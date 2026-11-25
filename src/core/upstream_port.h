#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "kraken2_data.h"
#include "kv_store.h"
#include "readcounts.h"
#include "taxonomy.h"

/**
 * The part of the server that follows kraken2's classification algorithm.
 *
 * kraken2's own classify program keeps these as functions private to its
 * classify.cc, tangled up with its file and stream handling, so they cannot be
 * linked; they are copied here. This is the ONE place that has to be updated
 * when kraken2 changes how it classifies. Everything else in the server uses
 * classification only through core/classifier.h.
 *
 * Copied from kraken2 commit 8d0e7ad (v2.18.0-RC2), src/classify.cc:
 *   MinimizerLookups   <- struct MinimizerLookups
 *   ResolveTree        <- ResolveTree()
 * Left out: quick mode, paired-end reads and unique-minimizer flagging, none
 * of which the server offers. The hitlist formatting is our own (AppendHitlist
 * in classifier.cc), as upstream's writes straight into its output buffer.
 *
 * tests/integration/compare_with_kraken2.sh checks that the results still match upstream.
 */
namespace k2s {
namespace upstream {

// Markers in a run-length encoded list of taxa, above every real taxon id
static const kraken2::taxid_t MATE_PAIR_BORDER_TAXON = kraken2::TAXID_MAX;
static const kraken2::taxid_t READING_FRAME_BORDER_TAXON = kraken2::TAXID_MAX - 1;
static const kraken2::taxid_t AMBIGUOUS_SPAN_TAXON = kraken2::TAXID_MAX - 2;

/**
 * Looks minimizers up in the hash in batches, and builds up from the results
 * what the classification is decided on: the hit count for each taxon, how many
 * distinct minimizers hit (hit groups), and the run-length encoded list of taxa
 * from which the hitlist is printed, as (taxon, count) pairs.
 */
struct MinimizerLookups
{
    MinimizerLookups(const kraken2::KeyValueStore *hash, std::vector<kraken2::taxid_t> &taxa,
                     kraken2::taxon_counters_t &counters, kraken2::taxon_counts_t &hit_counts,
                     std::vector<kraken2::LookupData> &lookup_buffer,
                     bool write_minimizer_data);

    void add_data(uint64_t minimizer, kraken2::taxid_t taxid);
    void get_batch();

    const kraken2::KeyValueStore *_hash;
    std::vector<kraken2::taxid_t> &_taxa;
    kraken2::taxon_counters_t &_taxon_counters;
    kraken2::taxon_counts_t &_hit_counts;
    std::vector<kraken2::LookupData> &lookup_keys;
    int64_t _minimizer_hit_groups;
    uint64_t _previous_minimizer;
    bool _write_minimizer_data;
    static const size_t batch_size = 1024;
};

/// The taxon a read is assigned to, given how many hits each taxon had. 0 means
/// unclassified.
kraken2::taxid_t ResolveTree(kraken2::taxon_counts_t &hit_counts,
    const kraken2::Taxonomy &taxonomy, size_t total_minimizers, double confidence_threshold);

}  // namespace upstream
}  // namespace k2s
