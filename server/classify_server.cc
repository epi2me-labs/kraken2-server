#include <fstream>
#include <getopt.h>
#include <thread>
#include <sysexits.h>

#include "classify_server.h"
#include "messages.h"

using namespace std::chrono_literals; // ns, us, ms, s, h, etc.

Kraken2ServerClassifier::Kraken2ServerClassifier(Options &options)
        : opts(options),
            taxonomy(opts.taxonomy_filename, opts.use_memory_mapping),
            hash(opts.index_filename, opts.use_memory_mapping) {
    // start a thread pool to handle classification tasks and
    // start loading the index. Should probably do better to
    // handle errors in LoadIndex
    std::cout << "Creating classification thread pool with "
              << opts.thread_pool << " thread(s)." << std::endl;
    pool.reset(opts.thread_pool);
    std::thread loader([this]() { LoadIndex(); });
    loader.detach();
}


Kraken2ServerClassifier::~Kraken2ServerClassifier(){
    // Any deconstruction
}


const char *Kraken2ServerClassifier::GetSummary() { return summary.c_str(); }


void Kraken2ServerClassifier::LoadIndex() {
    index_available = false;
    std::cerr << "Loading database information..." << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(opts.wait));

    try {
        idx_opts = {0};
        ifstream idx_opt_fs(opts.options_filename);
        struct stat sb;
        if (stat(opts.options_filename.c_str(), &sb) < 0)
            throw std::runtime_error("Unable to get filesize of index file.");
        auto opts_filesize = sb.st_size;
        idx_opt_fs.read((char *)&idx_opts, opts_filesize);
        opts.use_translated_search = !idx_opts.dna_db;
    }
    catch (const std::exception &ex) {
        std::cerr << "Unable to load index"
                  << ": " << ex.what() << std::endl;
        index_broken = true;
        return;
    }
    std::cerr << "Successfully loaded index." << std::endl;
    index_available = true;
}

void ResultsHandler(
        ServerStream *stream, std::future<void> finish,
        taxon_counters_t &stream_taxon_counters,
        ClassificationStats &stream_stats,
        ThreadSafeQueue<BatchResults> *results_queue) {
    while (finish.wait_for(0s) == std::future_status::timeout) {
        std::optional<BatchResults> res = results_queue->pop();
        if (res.has_value()) {
            // put the results in the stream
            Kraken2SequenceStreamResult result;
            *(result.mutable_classifications()) = res->k2results;
            stream->Write(result, WriteOptions().set_buffer_hint()); 
            // update stats for the stream
            stream_stats.total_bases += res->stats.total_bases;
            stream_stats.total_classified += res->stats.total_classified;
            stream_stats.total_sequences += res->stats.total_sequences;
            // update taxon_counters for the stream
            for (auto &kv_pair : res->taxon_counters) {
                stream_taxon_counters[kv_pair.first] += std::move(kv_pair.second);
            }
        }
    }
}

void Kraken2ServerClassifier::ProcessSequenceStream(
        ServerContext *context, ServerStream *stream, std::string &results) {
    std::cerr << "Starting stream handler." << std::endl;
    
    // Stats for the whole stream
    taxon_counters_t stream_taxon_counters;
    ClassificationStats stream_stats = {0, 0, 0};

    struct timeval tv1, tv2;
    gettimeofday(&tv1, nullptr);

    // create a queue and associated thread to aggregate the results of batches
    // and post to our output stream
    ThreadSafeQueue<BatchResults> *results_queue = new ThreadSafeQueue<BatchResults>();
    std::promise<void> complete;
    std::future<void> batches_complete = complete.get_future();
    std::thread results_thread(ResultsHandler,
        stream, std::move(batches_complete),
        std::ref(stream_taxon_counters), std::ref(stream_stats), results_queue);

    // Classify while reads are still being received on the input stream
    Kraken2SequenceRequestMulti req;
    std::vector<Kraken2SequenceRequest> seq_batch;
    std::vector<std::future<bool>> futures;
    while (!context->IsCancelled() && stream->Read(&req)) {
        // We could rebatch here, for now just pass the batch as is.
        futures.push_back(
            pool.submit(
                &Kraken2ServerClassifier::ProcessBatch, this,
                std::move(req), results_queue));
    }

    // wait for all futures to resolve, then wait for the queue to be empty,
    // and finally tell the results thread to finish
    for (auto &fut : futures) { fut.get(); }
    while (results_queue->size() > 0) {}
    complete.set_value();
    results_thread.join();

    gettimeofday(&tv2, nullptr);
    // generate the report, and update servers total history
    GenerateReport(
        results, summary, opts, taxonomy, tv1, tv2, stream_stats, total_stats,
        stream_taxon_counters, total_taxon_counters, stats_mtx);

    delete results_queue;
    std::cerr << "Finished stream handler." << std::endl;
}


bool Kraken2ServerClassifier::ProcessBatch(
    Kraken2SequenceRequestMulti reqs,
    ThreadSafeQueue<BatchResults> *result_q) {

    MinimizerScanner scanner(
        idx_opts.k, idx_opts.l, idx_opts.spaced_seed_mask,
        idx_opts.dna_db, idx_opts.toggle_mask,
        idx_opts.revcom_version);
    vector<taxid_t> taxa;
    taxon_counts_t hit_counts;
    vector<string> translated_frames(6);

    BatchResults results = BatchResults();

    kraken2::Sequence seq;
    for (auto &req : reqs.seqs()) {
        SequenceRequestToSequence(req, seq);
        results.stats.total_sequences++;
        results.stats.total_bases += seq.seq.size();
        if (opts.minimum_quality_score > 0)
            MaskLowQualityBases(seq, opts.minimum_quality_score);

        Kraken2SequenceResult classification = ClassifySequence(
            seq, hash, taxonomy, idx_opts, opts, results.stats, scanner,
            taxa, hit_counts, translated_frames, results.taxon_counters);

        results.k2results.mutable_classes()->Add(std::move(classification));
    }

    result_q->push(std::move(results));
    return true;
}


////////////////////////////////
// The following methods are adapted from the Kraken2 source code.
// Paired end and quick mode logic has been removed.
////////////////////////////////

void Kraken2ServerClassifier::AddHitlistString(
    ostringstream &oss, vector<taxid_t> &taxa, Taxonomy &taxonomy)
{
    auto last_code = taxa[0];
    auto code_count = 1;

    for (size_t i = 1; i < taxa.size(); i++)
    {
        auto code = taxa[i];

        if (code == last_code)
        {
            code_count += 1;
        }
        else
        {
            if (last_code != MATE_PAIR_BORDER_TAXON && last_code != READING_FRAME_BORDER_TAXON)
            {
                if (last_code == AMBIGUOUS_SPAN_TAXON)
                {
                    oss << "A:" << code_count << " ";
                }
                else
                {
                    auto ext_code = taxonomy.nodes()[last_code].external_id;
                    oss << ext_code << ":" << code_count << " ";
                }
            }
            else
            { // mate pair/reading frame marker
                oss << (last_code == MATE_PAIR_BORDER_TAXON ? "|:| " : "-:- ");
            }
            code_count = 1;
            last_code = code;
        }
    }
    if (last_code != MATE_PAIR_BORDER_TAXON && last_code != READING_FRAME_BORDER_TAXON)
    {
        if (last_code == AMBIGUOUS_SPAN_TAXON)
        {
            oss << "A:" << code_count << " ";
        }
        else
        {
            auto ext_code = taxonomy.nodes()[last_code].external_id;
            oss << ext_code << ":" << code_count;
        }
    }
    else
    { // mate pair/reading frame marker
        oss << (last_code == MATE_PAIR_BORDER_TAXON ? "|:|" : "-:-");
    }
}

Kraken2SequenceResult Kraken2ServerClassifier::ClassifySequence(
    Sequence &dna, CompactHashTable &hash, Taxonomy &taxonomy, IndexOptions &idx_opts,
    Options &opts, ClassificationStats &stats, MinimizerScanner &scanner,
    vector<taxid_t> &taxa, taxon_counts_t &hit_counts,
    vector<string> &tx_frames, taxon_counters_t &curr_taxon_counts)
{
    uint64_t *minimizer_ptr;
    taxid_t call = 0;
    taxa.clear();
    hit_counts.clear();
    auto frame_ct = opts.use_translated_search ? 6 : 1;
    int64_t minimizer_hit_groups = 0;

    if (opts.use_translated_search)
    {
        TranslateToAllFrames(dna.seq, tx_frames);
    }
    // index of frame is 0 - 5 w/ tx search (or 0 if no tx search)
    for (int frame_idx = 0; frame_idx < frame_ct; frame_idx++)
    {
        if (opts.use_translated_search)
        {
            scanner.LoadSequence(tx_frames[frame_idx]);
        }
        else
        {
            scanner.LoadSequence(dna.seq);
        }
        uint64_t last_minimizer = UINT64_MAX;
        taxid_t last_taxon = TAXID_MAX;
        while ((minimizer_ptr = scanner.NextMinimizer()) != nullptr)
        {
            taxid_t taxon;
            if (scanner.is_ambiguous())
            {
                taxon = AMBIGUOUS_SPAN_TAXON;
            }
            else
            {
                if (*minimizer_ptr != last_minimizer)
                {
                    bool skip_lookup = false;
                    if (idx_opts.minimum_acceptable_hash_value)
                    {
                        if (MurmurHash3(*minimizer_ptr) < idx_opts.minimum_acceptable_hash_value)
                            skip_lookup = true;
                    }
                    taxon = 0;
                    if (!skip_lookup)
                        taxon = hash.Get(*minimizer_ptr);
                    last_taxon = taxon;
                    last_minimizer = *minimizer_ptr;
                    // Increment this only if (a) we have DB hit and
                    // (b) minimizer != last minimizer

                    if (taxon)
                    {
                        minimizer_hit_groups++;
                        // New minimizer should trigger registering minimizer in RC/HLL
                        curr_taxon_counts[taxon].add_kmer(scanner.last_minimizer());
                    }
                }
                else
                {
                    taxon = last_taxon;
                }
                if (taxon)
                {
                    hit_counts[taxon]++;
                }
            }
            taxa.push_back(taxon);
        }
        if (opts.use_translated_search && frame_idx != 5)
            taxa.push_back(READING_FRAME_BORDER_TAXON);
    }

    delete minimizer_ptr;

    auto total_kmers = taxa.size();

    if (opts.use_translated_search) // account for reading frame markers
        total_kmers -= 2;
    call = ResolveTree(hit_counts, taxonomy, total_kmers, opts);
    // Void a call made by too few minimizer groups
    if (call && minimizer_hit_groups < opts.minimum_hit_groups)
        call = 0;

    if (call)
    {
        stats.total_classified++;
        curr_taxon_counts[call].incrementReadCount();
    }

    Kraken2SequenceResult result;
    result.set_id(dna.id);
    if (call)
    {
        result.set_classified(true);
        result.set_tax_id(taxonomy.nodes()[call].external_id);
        result.set_name(taxonomy.name_data() + taxonomy.nodes()[call].name_offset);
    }
    else
        result.set_classified(false);
    result.set_size(dna.seq.size());
    if (taxa.empty())
        result.set_hitlist("0:0");
    else
    {
        std::ostringstream hitlist;
        AddHitlistString(hitlist, taxa, taxonomy);
        result.set_hitlist(hitlist.str());
    }

    return result;
}

void Kraken2ServerClassifier::MaskLowQualityBases(Sequence &dna, int minimum_quality_score)
{
    if (dna.format != FORMAT_FASTQ)
        return;
    if (dna.seq.size() != dna.quals.size())
        errx(EX_DATAERR, "%s: Sequence length (%d) != Quality string length (%d)",
             dna.id.c_str(), (int)dna.seq.size(), (int)dna.quals.size());
    for (size_t i = 0; i < dna.seq.size(); i++)
    {
        if ((dna.quals[i] - '!') < minimum_quality_score)
            dna.seq[i] = 'x';
    }
}


taxid_t Kraken2ServerClassifier::ResolveTree(taxon_counts_t &hit_counts,
                                             Taxonomy &taxonomy, size_t total_minimizers, Options &opts)
{
    taxid_t max_taxon = 0;
    uint32_t max_score = 0;
    uint32_t required_score = ceil(opts.confidence_threshold * total_minimizers);

    // Sum each taxon's LTR path, find taxon with highest LTR score
    for (auto &kv_pair : hit_counts)
    {
        taxid_t taxon = kv_pair.first;
        uint32_t score = 0;

        for (auto &kv_pair2 : hit_counts)
        {
            taxid_t taxon2 = kv_pair2.first;

            if (taxonomy.IsAAncestorOfB(taxon2, taxon))
            {
                score += kv_pair2.second;
            }
        }

        if (score > max_score)
        {
            max_score = score;
            max_taxon = taxon;
        }
        else if (score == max_score)
        {
            max_taxon = taxonomy.LowestCommonAncestor(max_taxon, taxon);
        }
    }

    // Reset max. score to be only hits at the called taxon
    max_score = hit_counts[max_taxon];
    // We probably have a call w/o required support (unless LCA resolved tie)
    while (max_taxon && max_score < required_score)
    {
        max_score = 0;
        for (auto &kv_pair : hit_counts)
        {
            taxid_t taxon = kv_pair.first;
            // Add to score if taxon in max_taxon's clade
            if (taxonomy.IsAAncestorOfB(max_taxon, taxon))
            {
                max_score += kv_pair.second;
            }
        }
        // Score is now sum of hits at max_taxon and w/in max_taxon clade
        if (max_score >= required_score)
            // Kill loop and return, we've got enough support here
            return max_taxon;
        else
            // Run up tree until confidence threshold is met
            // Run off tree if required score isn't met
            max_taxon = taxonomy.nodes()[max_taxon].parent_id;
    }

    return max_taxon;
}

std::string Kraken2ServerClassifier::ReportStats(struct timeval time1, struct timeval time2,
                                                 ClassificationStats &stats)
{
    time2.tv_usec -= time1.tv_usec;
    time2.tv_sec -= time1.tv_sec;
    if (time2.tv_usec < 0)
    {
        time2.tv_sec--;
        time2.tv_usec += 1000000;
    }
    double seconds = time2.tv_usec;
    seconds /= 1e6;
    seconds += time2.tv_sec;

    uint64_t total_unclassified = stats.total_sequences - stats.total_classified;

    return std::to_string(stats.total_sequences) + " sequences (" + DoubleStatToString(stats.total_bases / 1.0e6, 2) + " Mbp) processed in " + DoubleStatToString(seconds, 2) + "s (" + DoubleStatToString(stats.total_sequences / 1.0e3 / (seconds / 60), 2) + " Kseq/m, " + DoubleStatToString(stats.total_bases / 1.0e6 / (seconds / 60), 2) + " Mbp/m).\n" +
           "\t" + std::to_string(stats.total_classified) + " sequences classified (" + DoubleStatToString(stats.total_classified * 100.0 / stats.total_sequences, 2) + "%)\n" +
           "\t" + std::to_string(total_unclassified) + " sequences unclassified (" + DoubleStatToString(total_unclassified * 100.0 / stats.total_sequences, 2) + "%)\n";
}

std::string Kraken2ServerClassifier::ReportTotalStats(ClassificationStats &stats)
{
    uint64_t total_unclassified = stats.total_sequences - stats.total_classified;

    return std::to_string(stats.total_sequences) + " sequences (" + DoubleStatToString(stats.total_bases / 1.0e6, 2) + " Mbp) processed.\n" +
           std::to_string(stats.total_classified) + " sequences classified (" + DoubleStatToString(stats.total_classified * 100.0 / stats.total_sequences, 2) + "%).\n" +
           std::to_string(total_unclassified) + " sequences unclassified (" + DoubleStatToString(total_unclassified * 100.0 / stats.total_sequences, 2) + "%).\n";
}

void Kraken2ServerClassifier::GenerateReport(
        std::string &results, std::string &summary, Options &opts, Taxonomy &taxonomy,
        timeval &tv1, timeval &tv2, ClassificationStats &stats,
        ClassificationStats &total_stats, taxon_counters_t &taxon_counters, taxon_counters_t &total_taxon_counters,
        std::mutex &stats_mtx)
{
    std::ostringstream ss;
    auto total_unclassified = stats.total_sequences - stats.total_classified;
    ReportKrakenStyle(ss,
                      opts.report_zero_counts,
                      opts.report_kmer_data,
                      taxonomy,
                      taxon_counters,
                      stats.total_sequences,
                      total_unclassified);
    results.assign(ss.str());

    std::cerr << ReportStats(tv1, tv2, stats) << std::endl;

    if (opts.stats)
    {
        stats_mtx.lock();

        total_stats.total_sequences += stats.total_sequences;
        total_stats.total_classified += stats.total_classified;
        total_stats.total_bases += stats.total_bases;
        for (auto &kv_pair : taxon_counters)
        {
            total_taxon_counters[kv_pair.first] += std::move(kv_pair.second);
        }
        ss.str(std::string());
        total_unclassified = total_stats.total_sequences - total_stats.total_classified;
        ReportKrakenStyle(ss,
                          opts.report_zero_counts,
                          opts.report_kmer_data,
                          taxonomy,
                          total_taxon_counters,
                          total_stats.total_sequences,
                          total_unclassified);

        ss << "\n"
           << ReportTotalStats(total_stats);
        summary.assign(ss.str());

        stats_mtx.unlock();
    }
}

std::string Kraken2ServerClassifier::TrimPairInfo(std::string &id)
{
    size_t sz = id.size();
    if (sz <= 2)
        return id;
    if (id[sz - 2] == '/' && (id[sz - 1] == '1' || id[sz - 1] == '2'))
        return id.substr(0, sz - 2);
    return id;
}

std::string Kraken2ServerClassifier::DoubleStatToString(double d, const int precision)
{
    std::stringstream stream;
    stream << std::fixed << std::setprecision(precision) << d;
    return stream.str();
}