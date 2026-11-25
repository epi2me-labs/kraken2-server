#include "core/classifier.h"

#include <stdexcept>

#include "core/upstream_port.h"
#include "utilities.h"

using kraken2::taxid_t;

namespace k2s {

Classifier::Classifier(const Index &index, const ClassifyParams &params)
    : index_(index),
      params_(params),
      scanner_(index.options().k, index.options().l, index.options().spaced_seed_mask,
               index.options().dna_db, index.options().toggle_mask,
               index.options().revcom_version),
      frames_(6)
{
}

bool Classifier::Classify(const Read &read, Classification &out,
                          kraken2::taxon_counters_t &counters)
{
    const kraken2::IndexOptions &idx = index_.options();
    const kraken2::Taxonomy &taxonomy = index_.taxonomy();

    // Bases with a poor quality are replaced by one the scanner treats as ambiguous
    const char *bases = read.seq.data();
    if (params_.minimum_quality_score > 0 && read.fastq) {
        if (read.quals.size() != read.seq.size())
            throw std::invalid_argument(
                std::string(read.id) + ": sequence length (" + std::to_string(read.seq.size()) +
                ") != quality string length (" + std::to_string(read.quals.size()) + ")");
        masked_.assign(read.seq);
        for (size_t i = 0; i < masked_.size(); i++) {
            if ((read.quals[i] - '!') < params_.minimum_quality_score)
                masked_[i] = 'x';
        }
        bases = masked_.data();
    }

    out.hit_runs.clear();
    hit_counts_.clear();
    const int frame_count = index_.translated() ? 6 : 1;
    size_t total_kmers = 0;

    upstream::MinimizerLookups lookups(
        &index_.hash(), out.hit_runs, counters, hit_counts_, lookup_buffer_,
        params_.report_kmer_data);

    if (index_.translated())
        kraken2::TranslateToAllFrames(bases, read.seq.size(), frames_);

    // index of frame is 0 - 5 w/ translated search (or 0 without)
    for (int frame = 0; frame < frame_count; frame++) {
        if (index_.translated())
            scanner_.LoadSequence(frames_[frame]);
        else
            scanner_.LoadSequence(bases, read.seq.size());

        uint64_t *minimizer;
        while ((minimizer = scanner_.NextMinimizer()) != nullptr) {
            if (scanner_.is_ambiguous()) {
                lookups.add_data(UINT64_MAX, upstream::AMBIGUOUS_SPAN_TAXON);
            }
            else {
                if (idx.minimum_acceptable_hash_value &&
                    kraken2::MurmurHash3(*minimizer) < idx.minimum_acceptable_hash_value)
                    *minimizer = UINT64_MAX;
                lookups.add_data(*minimizer, 0);
            }
            total_kmers += 1;
        }
        lookups.get_batch();
        if (index_.translated() && frame != 5)
            lookups.add_data(UINT64_MAX, upstream::READING_FRAME_BORDER_TAXON);
    }

    // The report needs every taxon with a hit to have a counter, even when no
    // k-mer data is kept.
    if (!params_.report_kmer_data) {
        for (auto &hit : hit_counts_)
            counters[hit.first];
    }

    taxid_t call = upstream::ResolveTree(hit_counts_, taxonomy, total_kmers,
                                         params_.confidence_threshold);
    // Void a call made by too few minimizer groups
    if (call && lookups._minimizer_hit_groups < params_.minimum_hit_groups)
        call = 0;

    out.taxon = call;
    if (call)
        counters[call].incrementReadCount();
    return call != 0;
}

uint64_t Classifier::ExternalId(taxid_t taxon) const
{
    return index_.taxonomy().nodes()[taxon].external_id;
}

const char *Classifier::Name(taxid_t taxon) const
{
    const kraken2::Taxonomy &taxonomy = index_.taxonomy();
    return taxonomy.name_data() + taxonomy.nodes()[taxon].name_offset;
}

void Classifier::AppendHitlist(const Classification &c, std::string &out) const
{
    if (c.hit_runs.empty()) {  // a read too short to have any minimizers
        out += "0:0";
        return;
    }
    const size_t start = out.size();
    for (size_t i = 0; i + 1 < c.hit_runs.size(); i += 2) {
        const taxid_t code = c.hit_runs[i];
        const std::string count = std::to_string(c.hit_runs[i + 1]);
        if (code == upstream::MATE_PAIR_BORDER_TAXON ||
            code == upstream::READING_FRAME_BORDER_TAXON) {
            out += (code == upstream::MATE_PAIR_BORDER_TAXON ? "|:| " : "-:- ");
        }
        else if (code == upstream::AMBIGUOUS_SPAN_TAXON) {
            out += "A:" + count + " ";
        }
        else {
            out += std::to_string(ExternalId(code)) + ":" + count + " ";
        }
    }
    if (out.size() > start && out.back() == ' ')
        out.pop_back();
}

}  // namespace k2s
