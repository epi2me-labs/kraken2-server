#include "report_server.h"

namespace kraken2
{

  taxon_counts_t GetCladeCounts(Taxonomy &tax, taxon_counts_t &call_counts)
  {
    taxon_counts_t clade_counts;

    for (auto &kv_pair : call_counts)
    {
      auto taxid = kv_pair.first;
      auto count = kv_pair.second;

      while (taxid)
      {
        clade_counts[taxid] += count;
        taxid = tax.nodes()[taxid].parent_id;
      }
    }

    return clade_counts;
  }

  taxon_counters_t GetCladeCounters(Taxonomy &tax, taxon_counters_t &call_counters)
  {
    taxon_counters_t clade_counters;

    for (auto &kv_pair : call_counters)
    {
      auto taxid = kv_pair.first;
      auto counter = kv_pair.second;

      while (taxid)
      {
        clade_counters[taxid] += counter;
        taxid = tax.nodes()[taxid].parent_id;
      }
    }

    return clade_counters;
  }

  void PrintKrakenStyleReportLine(ostringstream &ss, bool report_kmer_data,
                                  uint64_t total_seqs,
                                  READCOUNTER clade_counter, READCOUNTER taxon_counter,
                                  const string &rank_str, uint32_t taxid, const string &sci_name, int depth)
  {
    char pct_buffer[7] = "";
    snprintf(pct_buffer, 7, "%6.2f", 100.0 * clade_counter.readCount() / total_seqs);

    ss << pct_buffer << "\t"
       << clade_counter.readCount() << "\t"
       << taxon_counter.readCount() << "\t";
    if (report_kmer_data)
    {
      ss << clade_counter.kmerCount() << "\t"
         << clade_counter.distinctKmerCount() << "\t";
    }
    ss << rank_str << "\t"
       << taxid << "\t";
    for (auto i = 0; i < depth; i++)
      ss << "  ";
    ss << sci_name << std::endl;
  }

  // Depth-first search of taxonomy tree, reporting info at each node
  void KrakenReportDFS(uint32_t taxid, ostringstream &ss, bool report_zeros,
                       bool report_kmer_data,
                       Taxonomy &taxonomy, taxon_counters_t &clade_counters,
                       taxon_counters_t &call_counters, uint64_t total_seqs,
                       char rank_code, int rank_depth, int depth)
  {
    // Clade count of 0 means all subtree nodes have clade count of 0
    if (!report_zeros && clade_counters[taxid].readCount() == 0)
      return;
    TaxonomyNode node = taxonomy.nodes()[taxid];
    string rank = taxonomy.rank_data() + node.rank_offset;

    if (rank == "superkingdom")
    {
      rank_code = 'D';
      rank_depth = 0;
    }
    else if (rank == "kingdom")
    {
      rank_code = 'K';
      rank_depth = 0;
    }
    else if (rank == "phylum")
    {
      rank_code = 'P';
      rank_depth = 0;
    }
    else if (rank == "class")
    {
      rank_code = 'C';
      rank_depth = 0;
    }
    else if (rank == "order")
    {
      rank_code = 'O';
      rank_depth = 0;
    }
    else if (rank == "family")
    {
      rank_code = 'F';
      rank_depth = 0;
    }
    else if (rank == "genus")
    {
      rank_code = 'G';
      rank_depth = 0;
    }
    else if (rank == "species")
    {
      rank_code = 'S';
      rank_depth = 0;
    }
    else
    {
      rank_depth++;
    }

    string rank_str(&rank_code, 0, 1);
    if (rank_depth != 0)
      rank_str += std::to_string(rank_depth);

    string name = taxonomy.name_data() + node.name_offset;

    PrintKrakenStyleReportLine(ss, report_kmer_data, total_seqs,
                               clade_counters[taxid], call_counters[taxid], rank_str, node.external_id,
                               name, depth);

    auto child_count = node.child_count;
    if (child_count != 0)
    {
      vector<uint64_t> children(child_count);

      for (auto i = 0u; i < child_count; i++)
      {
        children[i] = node.first_child + i;
      }
      // Sorting child IDs by descending order of clade read counts
      std::sort(children.begin(), children.end(),
                [&](const uint64_t &a, const uint64_t &b)
                {
                  return clade_counters[a].readCount() > clade_counters[b].readCount();
                });
      for (auto child : children)
      {
        KrakenReportDFS(child, ss, report_zeros, report_kmer_data, taxonomy,
                        clade_counters, call_counters, total_seqs, rank_code, rank_depth,
                        depth + 1);
      }
    }
  }

  void ReportKrakenStyle(ostringstream &ss, bool report_zeros, bool report_kmer_data,
                         Taxonomy &taxonomy, taxon_counters_t &call_counters, uint64_t total_seqs,
                         uint64_t total_unclassified)
  {
    taxon_counters_t clade_counters = GetCladeCounters(taxonomy, call_counters);

    ss << "\% of Seqs"
       << "\t"
       << "Clades"
       << "\t"
       << "Taxonomies"
       << "\t";
    if (report_kmer_data)
    {
      ss << "Kmers"
         << "\t"
         << "Distinct Kmers"
         << "\t";
    }
    ss << "Rank"
       << "\t"
       << "Taxonomy ID"
       << "\t"
       << "Scientific Name"
       << "\n";

    // Special handling of the unclassified sequences
    if (total_unclassified != 0 || report_zeros)
    {
      READCOUNTER rc(total_unclassified, 0);
      PrintKrakenStyleReportLine(ss, report_kmer_data, total_seqs, rc,
                                 rc, "U", 0, "unclassified", 0);
    }
    // DFS through the taxonomy, printing nodes as encountered
    KrakenReportDFS(1, ss, report_zeros, report_kmer_data, taxonomy,
                    clade_counters, call_counters, total_seqs, 'R', -1, 0);
  }
}