#include <cerrno>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <thread>
#include <sys/stat.h>
#include <unistd.h>

#include "classify_server.h"
#include "messages.h"

using namespace std::chrono_literals; // ns, us, ms, s, h, etc.

Kraken2ServerClassifier::Kraken2ServerClassifier(const Options &options)
        : opts(options) {
    std::cout << "Creating classification thread pool with "
              << opts.thread_pool << " thread(s)." << std::endl;
    pool.reset(opts.thread_pool);
    // Start loading the index, the server can accept connections meanwhile
    // and tell clients to wait.
    loader_ = std::thread([this]() { LoadIndex(); });
}


Kraken2ServerClassifier::~Kraken2ServerClassifier() {
    if (loader_.joinable())
        loader_.join();
}


std::string Kraken2ServerClassifier::GetSummary() const {
    std::lock_guard<std::mutex> lock(stats_mtx);
    return summary;
}


namespace
{
void RequireReadable(const std::string &filename) {
    if (access(filename.c_str(), R_OK) != 0)
        throw std::runtime_error("cannot read " + filename + ": " + strerror(errno));
}
} // namespace


void Kraken2ServerClassifier::LoadIndex() {
    std::cerr << "Loading database information..." << std::endl;
    // Delays loading, for testing clients that wait for the server.
    std::this_thread::sleep_for(std::chrono::seconds(opts.wait));

    try {
        RequireReadable(opts.options_filename);
        RequireReadable(opts.taxonomy_filename);
        RequireReadable(opts.index_filename);

        struct stat sb;
        if (stat(opts.options_filename.c_str(), &sb) < 0)
            throw std::runtime_error("unable to get size of " + opts.options_filename);
        if (static_cast<size_t>(sb.st_size) != sizeof(IndexOptions))
            throw std::runtime_error(
                opts.options_filename + " is not the size expected of a database options file");
        idx_opts = {0};
        std::ifstream idx_opt_fs(opts.options_filename, std::ios::binary);
        if (!idx_opt_fs.read(reinterpret_cast<char *>(&idx_opts), sizeof(idx_opts)))
            throw std::runtime_error("unable to read " + opts.options_filename);
        // The database dictates whether translated search is used.
        opts.use_translated_search = !idx_opts.dna_db;
        // Required before any translation: upstream's tables are empty until
        // this is called (kraken2's classify calls it at the start of a run).
        initLookUpTables();

        taxonomy.reset(new Taxonomy(opts.taxonomy_filename, opts.use_memory_mapping));
        // Upstream databases come with either 32 or 40 bit cells.
        switch (GetKVStoreCellType(opts.index_filename)) {
        case CompactHash32:
            hash.reset(new CompactHashTable<CompactHashCell>(
                opts.index_filename, opts.use_memory_mapping));
            break;
        case CompactHash40:
            hash.reset(new CompactHashTable<CompactHashCell40>(
                opts.index_filename, opts.use_memory_mapping));
            break;
        default:
            throw std::runtime_error("unable to determine width of compact hash cell");
        }
    }
    catch (const std::exception &ex) {
        std::cerr << "Unable to load index"
                  << ": " << ex.what() << std::endl;
        state_ = IndexState::Failed;
        return;
    }
    std::cerr << "Successfully loaded index." << std::endl;
    state_ = IndexState::Ready;
}


namespace
{
// Writes classifications back to the client as batches complete, and
// accumulates the stream's statistics. Runs until the queue is closed and
// drained.
void ResultsHandler(
        ServerStream *stream,
        ThreadSafeQueue<BatchResults> *results_queue,
        taxon_counters_t &stream_taxon_counters,
        ClassificationStats &stream_stats) {
    bool client_writable = true;
    while (auto res = results_queue->pop()) {
        if (client_writable) {
            // We're assuming the client can receive arbitrarily large messages.
            // That's fine for now as the client is set to receive INT_MAX.
            Kraken2SequenceStreamResult result;
            result.mutable_classifications()->Swap(&res->k2results);
            // Keep draining if the client has gone, so the statistics stay correct.
            client_writable = stream->Write(result, WriteOptions().set_buffer_hint());
        }
        stream_stats.total_bases += res->stats.total_bases;
        stream_stats.total_classified += res->stats.total_classified;
        stream_stats.total_sequences += res->stats.total_sequences;
        for (auto &kv_pair : res->taxon_counters) {
            stream_taxon_counters[kv_pair.first] += std::move(kv_pair.second);
        }
    }
}
} // namespace

grpc::Status Kraken2ServerClassifier::ProcessSequenceStream(
        ServerContext *context, ServerStream *stream, std::string &report) {
    std::cerr << "Starting stream handler." << std::endl;
    stream->SendInitialMetadata();

    // Stats for the whole stream
    taxon_counters_t stream_taxon_counters;
    ClassificationStats stream_stats = {0, 0, 0};

    struct timeval tv1, tv2;
    gettimeofday(&tv1, nullptr);

    // Batches are classified on the shared pool, and a thread aggregates the
    // results and posts them to the output stream.
    ThreadSafeQueue<BatchResults> results_queue;
    StreamFailure failure;
    std::thread results_thread(ResultsHandler,
        stream, &results_queue,
        std::ref(stream_taxon_counters), std::ref(stream_stats));

    std::vector<std::future<void>> futures;
    try {
        // Classify while reads are still being received on the input stream
        Kraken2SequenceRequestMulti req;
        while (!context->IsCancelled() && !failure.failed() && stream->Read(&req)) {
            // We could rebatch here, for now just pass the batch as is.
            futures.push_back(pool.submit(
                [this, batch = std::move(req), &results_queue, &failure]() mutable {
                    ProcessBatch(std::move(batch), &results_queue, &failure);
                }));
        }
        for (auto &fut : futures) { fut.get(); }
    }
    catch (...) {
        // The results thread must be joined before it is destroyed.
        for (auto &fut : futures) { if (fut.valid()) fut.wait(); }
        results_queue.close();
        results_thread.join();
        throw;
    }
    // Every batch is done, so the queue holds everything there will be.
    results_queue.close();
    results_thread.join();

    gettimeofday(&tv2, nullptr);
    if (failure.failed()) {
        std::cerr << "Stream failed: " << failure.message() << std::endl;
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, failure.message());
    }

    // generate the report, and update the server's total history
    GenerateReport(report, tv1, tv2, stream_stats, stream_taxon_counters);
    std::cerr << "Finished stream handler." << std::endl;
    return grpc::Status::OK;
}


void Kraken2ServerClassifier::ProcessBatch(
    Kraken2SequenceRequestMulti reqs,
    ThreadSafeQueue<BatchResults> *result_q,
    StreamFailure *failure) {
    try {
        if (failure->failed())
            return;

        MinimizerScanner scanner(
            idx_opts.k, idx_opts.l, idx_opts.spaced_seed_mask,
            idx_opts.dna_db, idx_opts.toggle_mask,
            idx_opts.revcom_version);
        vector<taxid_t> taxa;
        taxon_counts_t hit_counts;
        vector<string> translated_frames(6);

        BatchResults results;

        Sequence seq;
        for (auto &req : reqs.seqs()) {
            SequenceRequestToSequence(req, seq);
            results.stats.total_sequences++;
            results.stats.total_bases += seq.seq.size();
            if (opts.minimum_quality_score > 0)
                MaskLowQualityBases(seq, opts.minimum_quality_score);

            results.k2results.mutable_classes()->Add(ClassifySequence(
                seq, hash.get(), *taxonomy, idx_opts, opts, results.stats, scanner,
                taxa, hit_counts, translated_frames, results.taxon_counters));
        }

        result_q->push(std::move(results));
    }
    catch (const std::exception &ex) {
        failure->Set(ex.what());
    }
    catch (...) {
        failure->Set("unknown error while classifying");
    }
}


////////////////////////////////
// The following methods are adapted from the Kraken2 source code.
// Paired end and quick mode logic has been removed.
////////////////////////////////

void Kraken2ServerClassifier::AddHitlistString(
    std::string &oss, vector<taxid_t> &taxa, Taxonomy &taxonomy)
{
    // taxa holds run-length encoded (code, count) pairs
    for (size_t i = 0; i + 1 < taxa.size(); i += 2)
    {
        auto code = taxa[i];
        auto count = std::to_string(taxa[i + 1]);
        if (code != MATE_PAIR_BORDER_TAXON && code != READING_FRAME_BORDER_TAXON)
        {
            if (code == AMBIGUOUS_SPAN_TAXON)
            {
                oss += "A:" + count + " ";
            }
            else
            {
                oss += std::to_string(taxonomy.nodes()[code].external_id) + ":" + count + " ";
            }
        }
        else
        { // mate pair/reading frame marker
            oss += (code == MATE_PAIR_BORDER_TAXON ? "|:| " : "-:- ");
        }
    }
    if (!oss.empty() && oss.back() == ' ')
        oss.pop_back();
}

namespace
{
// Adapted from MinimizerLookups in kraken2/src/classify.cc (not exported in a
// header upstream). Minimizers are looked up in batches and the resulting
// taxa run-length encoded as (taxon, count) pairs. Quick mode and the
// unique-minimizer flag have been removed.
struct MinimizerLookups
{
    MinimizerLookups(KeyValueStore *hash, std::vector<taxid_t> &taxa,
                     taxon_counters_t &counters, taxon_counts_t &hit_counts,
                     std::vector<LookupData> &lookup_buffer,
                     bool write_minimizer_data)
        : _hash(hash), _taxa(taxa), _taxon_counters(counters),
          _hit_counts(hit_counts), lookup_keys(lookup_buffer),
          _minimizer_hit_groups(0), _previous_minimizer(UINT64_MAX),
          _write_minimizer_data(write_minimizer_data)
    {
        lookup_keys.reserve(batch_size);
    }

    inline void add_data(uint64_t minimizer, taxid_t taxid)
    {
        lookup_keys.push_back({minimizer, taxid});
        if (lookup_keys.size() == batch_size)
            get_batch();
    }

    inline void get_batch()
    {
        if (lookup_keys.empty())
            return;
        _hash->GetBatch(lookup_keys);
        taxid_t previous_taxid = TAXID_MAX;
        if (_taxa.size() > 0)
            previous_taxid = _taxa[_taxa.size() - 2];

        for (size_t i = 0; i < lookup_keys.size(); i++)
        {
            bool repeat = lookup_keys[i].key == _previous_minimizer;
            // A hit can repeat a minimizer that missed. A border, or a
            // minimizer the database sampled away, ends a run of repeats; an
            // ambiguous span does not.
            if (lookup_keys[i].key != UINT64_MAX)
                _previous_minimizer = lookup_keys[i].key;
            else if (lookup_keys[i].value != AMBIGUOUS_SPAN_TAXON)
                _previous_minimizer = UINT64_MAX;
            if (lookup_keys[i].key != UINT64_MAX && lookup_keys[i].value > 0)
            {
                _hit_counts[lookup_keys[i].value] += 1;
                if (!repeat)
                {
                    _minimizer_hit_groups += 1;
                    if (_write_minimizer_data)
                        _taxon_counters[lookup_keys[i].value].add_kmer(lookup_keys[i].key);
                }
                else if (_write_minimizer_data)
                {
                    _taxon_counters[lookup_keys[i].value].increaseKmerCount(1);
                }
            }
            taxid_t taxid = lookup_keys[i].value;
            if (previous_taxid != taxid)
            {
                _taxa.push_back(taxid);
                _taxa.push_back(1);
            }
            else
            {
                _taxa.back() += 1;
            }
            previous_taxid = taxid;
        }
        lookup_keys.resize(0);
    }

    KeyValueStore *_hash;
    std::vector<taxid_t> &_taxa;
    taxon_counters_t &_taxon_counters;
    taxon_counts_t &_hit_counts;
    std::vector<LookupData> &lookup_keys;
    int64_t _minimizer_hit_groups;
    uint64_t _previous_minimizer;
    bool _write_minimizer_data;
    static const size_t batch_size = 1024;
};
} // namespace

Kraken2SequenceResult Kraken2ServerClassifier::ClassifySequence(
    Sequence &dna, KeyValueStore *hash, Taxonomy &taxonomy, IndexOptions &idx_opts,
    Options &opts, ClassificationStats &stats, MinimizerScanner &scanner,
    vector<taxid_t> &taxa, taxon_counts_t &hit_counts,
    vector<string> &tx_frames, taxon_counters_t &curr_taxon_counts)
{
    uint64_t *minimizer_ptr;
    taxid_t call = 0;
    taxa.clear();
    hit_counts.clear();
    auto frame_ct = opts.use_translated_search ? 6 : 1;
    size_t total_kmers = 0;

    // Only the batch buffer outlives the read, so it is allocated once per thread.
    static thread_local std::vector<LookupData> lookup_buffer;
    MinimizerLookups lookups(
        hash, taxa, curr_taxon_counts, hit_counts, lookup_buffer,
        opts.report_kmer_data);

    if (opts.use_translated_search)
    {
        TranslateToAllFrames(dna.seq.data(), dna.seq.size(), tx_frames);
    }
    // index of frame is 0 - 5 w/ tx search (or 0 if no tx search)
    for (int frame_idx = 0; frame_idx < frame_ct; frame_idx++)
    {
        if (opts.use_translated_search)
            scanner.LoadSequence(tx_frames[frame_idx]);
        else
            scanner.LoadSequence(dna.seq.data(), dna.seq.size());

        while ((minimizer_ptr = scanner.NextMinimizer()) != nullptr)
        {
            if (scanner.is_ambiguous())
            {
                lookups.add_data(UINT64_MAX, AMBIGUOUS_SPAN_TAXON);
            }
            else
            {
                if (idx_opts.minimum_acceptable_hash_value)
                {
                    if (MurmurHash3(*minimizer_ptr) < idx_opts.minimum_acceptable_hash_value)
                        *minimizer_ptr = UINT64_MAX;
                }
                lookups.add_data(*minimizer_ptr, 0);
            }
            total_kmers += 1;
        }
        lookups.get_batch();
        if (opts.use_translated_search && frame_idx != 5)
            lookups.add_data(UINT64_MAX, READING_FRAME_BORDER_TAXON);
    }

    // The report always needs every taxon with a hit to have a counter, even
    // when no k-mer data is kept.
    if (!lookups._write_minimizer_data)
    {
        for (auto &kv_pair : hit_counts)
            curr_taxon_counts[kv_pair.first];
    }

    call = ResolveTree(hit_counts, taxonomy, total_kmers, opts);
    // Void a call made by too few minimizer groups
    if (call && lookups._minimizer_hit_groups < opts.minimum_hit_groups)
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
        std::string hitlist;
        AddHitlistString(hitlist, taxa, taxonomy);
        result.set_hitlist(hitlist);
    }

    return result;
}

void Kraken2ServerClassifier::MaskLowQualityBases(Sequence &dna, int minimum_quality_score)
{
    if (dna.format != FORMAT_FASTQ)
        return;
    if (dna.seq.size() != dna.quals.size())
        throw std::invalid_argument(
            dna.id + ": sequence length (" + std::to_string(dna.seq.size()) +
            ") != quality string length (" + std::to_string(dna.quals.size()) + ")");
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
  const size_t hit_count = hit_counts.size();

  // With one hit, every ancestor has exactly the same clade support. No
  // parent walk or confidence scratch allocation can change the answer.
  if (hit_count == 1) {
    const auto &hit = *hit_counts.begin();
    return hit.first && static_cast<uint32_t>(hit.second) >= required_score
        ? hit.first : 0;
  }

  // Parents have smaller internal IDs. Stop a candidate's parent walk once
  // it is below every hit ID. For two hits, one ancestor test per candidate
  // avoids a hash lookup at each intervening parent on a long sparse chain.
  taxid_t min_hit_taxon = TAXID_MAX;
  if (hit_count > 2)
    for (const auto &hit : hit_counts)
      if (hit.first && hit.first < min_hit_taxon)
        min_hit_taxon = hit.first;

  // Sum direct hits on each candidate's parent chain instead of comparing
  // every pair of hit taxa. Expected O(DH), with no extra scoring storage.
  // Keep candidate iteration order and uint32_t score arithmetic unchanged.
  for (auto &kv_pair : hit_counts) {
    taxid_t taxon = kv_pair.first;
    uint32_t score = taxon ? kv_pair.second : 0;
    if (hit_count == 2) {
      for (const auto &hit : hit_counts)
        if (hit.first != taxon && taxonomy.IsAAncestorOfB(hit.first, taxon))
          score += hit.second;
    } else {
      for (taxid_t ancestor = taxonomy.nodes()[taxon].parent_id;
           ancestor && ancestor >= min_hit_taxon;
           ancestor = taxonomy.nodes()[ancestor].parent_id) {
        const auto hit = hit_counts.find(ancestor);
        if (hit != hit_counts.end())
          score += hit->second;
      }
    }

    if (score > max_score) {
      max_score = score;
      max_taxon = taxon;
    }
    else if (score == max_score) {
      max_taxon = taxonomy.LowestCommonAncestor(max_taxon, taxon);
    }
  }

  // Reset max. score to be only hits at the called taxon
  max_score = hit_counts[max_taxon];
  if (!max_taxon || max_score >= required_score)
    return max_taxon;

  // With two hits, support changes at no more than two intersections. Keep
  // those on the stack instead of allocating a whole ancestor chain.
  if (hit_count == 2) {
    std::pair<taxid_t, uint32_t> junctions[2] = {};
    size_t count = 0;
    for (const auto &hit : hit_counts) {
      // operator[] above may have inserted a third, zero-valued candidate.
      if (hit.first && hit.second)
        junctions[count++] = {taxonomy.LowestCommonAncestor(max_taxon, hit.first),
                              static_cast<uint32_t>(hit.second)};
    }
    if (count == 2 && junctions[0].first < junctions[1].first)
      std::swap(junctions[0], junctions[1]);
    if (count == 2 && junctions[0].first == junctions[1].first) {
      junctions[0].second += junctions[1].second;
      count = 1;
    }
    max_score = 0;
    for (size_t i = 0; i < count; ++i) {
      max_score += junctions[i].second;
      if (max_score >= required_score)
        return junctions[i].first;
    }
    return 0;
  }

  // Only the called taxon's ancestor chain can be returned by confidence
  // fallback. Bucket each hit at its first intersection with that chain:
  // LCA(call, hit). A prefix sum then gives every ancestor's clade support.
  // This avoids rescanning all hits at each ancestor: O(DH + H) time and
  // O(H) temporary space, allocated only when confidence needs checking.
  std::vector<std::pair<taxid_t, uint32_t>> lineage;
  for (taxid_t ancestor = max_taxon; ancestor;
       ancestor = taxonomy.nodes()[ancestor].parent_id)
    lineage.emplace_back(ancestor, 0);

  for (const auto &hit : hit_counts) {
    if (!hit.first)
      continue;  // 0 is not a taxon, despite LCA(x, 0) == x.
    const taxid_t intersection = taxonomy.LowestCommonAncestor(max_taxon, hit.first);
    // The lineage is strictly decreasing in internal ID.
    const auto bucket = std::lower_bound(lineage.begin(), lineage.end(), intersection,
        [](const std::pair<taxid_t, uint32_t> &entry, taxid_t id) {
          return entry.first > id;
        });
    bucket->second += hit.second;
  }

  max_score = 0;
  for (const auto &entry : lineage) {
    max_score += entry.second;
    if (max_score >= required_score)
      return entry.first;
  }

  return 0;
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
        std::string &report, timeval &tv1, timeval &tv2,
        ClassificationStats &stats, taxon_counters_t &taxon_counters)
{
    std::ostringstream ss;
    ReportKrakenStyle(ss, opts.report_zero_counts, opts.report_kmer_data,
                      *taxonomy, taxon_counters, stats.total_sequences,
                      stats.total_sequences - stats.total_classified);
    report = ss.str();

    std::cerr << ReportStats(tv1, tv2, stats) << std::endl;

    if (!opts.stats)
        return;

    std::lock_guard<std::mutex> lock(stats_mtx);
    total_stats.total_sequences += stats.total_sequences;
    total_stats.total_classified += stats.total_classified;
    total_stats.total_bases += stats.total_bases;
    for (auto &kv_pair : taxon_counters)
    {
        total_taxon_counters[kv_pair.first] += std::move(kv_pair.second);
    }
    std::ostringstream total;
    ReportKrakenStyle(total, opts.report_zero_counts, opts.report_kmer_data,
                      *taxonomy, total_taxon_counters, total_stats.total_sequences,
                      total_stats.total_sequences - total_stats.total_classified);
    total << "\n" << ReportTotalStats(total_stats);
    summary = total.str();
}

std::string Kraken2ServerClassifier::DoubleStatToString(double d, const int precision)
{
    std::stringstream stream;
    stream << std::fixed << std::setprecision(precision) << d;
    return stream.str();
}