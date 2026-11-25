#include "core/upstream_port.h"

#include <algorithm>
#include <cmath>

// See upstream_port.h. Names below are kraken2's; the code is kept as close to
// upstream's as possible so that updating it is a matter of diffing.
using kraken2::LookupData;
using kraken2::taxid_t;
using kraken2::TAXID_MAX;

namespace k2s {
namespace upstream {

MinimizerLookups::MinimizerLookups(
    const kraken2::KeyValueStore *hash, std::vector<taxid_t> &taxa,
    kraken2::taxon_counters_t &counters, kraken2::taxon_counts_t &hit_counts,
    std::vector<LookupData> &lookup_buffer, bool write_minimizer_data)
    : _hash(hash), _taxa(taxa), _taxon_counters(counters),
      _hit_counts(hit_counts), lookup_keys(lookup_buffer),
      _minimizer_hit_groups(0), _previous_minimizer(UINT64_MAX),
      _write_minimizer_data(write_minimizer_data)
{
    lookup_keys.reserve(batch_size);
}

void MinimizerLookups::add_data(uint64_t minimizer, taxid_t taxid)
{
    lookup_keys.push_back({minimizer, taxid});
    if (lookup_keys.size() == batch_size)
        get_batch();
}

void MinimizerLookups::get_batch()
{
    if (lookup_keys.empty())
        return;
    _hash->GetBatch(lookup_keys);
    taxid_t previous_taxid = TAXID_MAX;
    if (_taxa.size() > 0)
        previous_taxid = _taxa[_taxa.size() - 2];

    for (size_t i = 0; i < lookup_keys.size(); i++) {
        bool repeat = lookup_keys[i].key == _previous_minimizer;
        // A hit can repeat a minimizer that missed. A border, or a minimizer
        // the database sampled away, ends a run of repeats; an ambiguous span
        // does not.
        if (lookup_keys[i].key != UINT64_MAX)
            _previous_minimizer = lookup_keys[i].key;
        else if (lookup_keys[i].value != AMBIGUOUS_SPAN_TAXON)
            _previous_minimizer = UINT64_MAX;
        if (lookup_keys[i].key != UINT64_MAX && lookup_keys[i].value > 0) {
            _hit_counts[lookup_keys[i].value] += 1;
            if (!repeat) {
                _minimizer_hit_groups += 1;
                if (_write_minimizer_data)
                    _taxon_counters[lookup_keys[i].value].add_kmer(lookup_keys[i].key);
            }
            else if (_write_minimizer_data) {
                _taxon_counters[lookup_keys[i].value].increaseKmerCount(1);
            }
        }
        taxid_t taxid = lookup_keys[i].value;
        // run-length encode
        if (previous_taxid != taxid) {
            _taxa.push_back(taxid);
            _taxa.push_back(1);
        }
        else {
            _taxa.back() += 1;
        }
        previous_taxid = taxid;
    }
    lookup_keys.resize(0);
}

kraken2::taxid_t ResolveTree(kraken2::taxon_counts_t &hit_counts,
    const kraken2::Taxonomy &taxonomy, size_t total_minimizers, double confidence_threshold)
{
  taxid_t max_taxon = 0;
  uint32_t max_score = 0;
  uint32_t required_score = ceil(confidence_threshold * total_minimizers);
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

}  // namespace upstream
}  // namespace k2s
