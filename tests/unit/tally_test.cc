#include <thread>
#include <vector>

#include "check.h"
#include "core/tally.h"

using namespace k2s;

static Tally MakeTally(uint64_t sequences, uint64_t bases, uint64_t classified,
                       kraken2::taxid_t taxon, uint64_t reads)
{
    Tally t;
    t.stats.sequences = sequences;
    t.stats.bases = bases;
    t.stats.classified = classified;
    for (uint64_t i = 0; i < reads; ++i)
        t.counters[taxon].incrementReadCount();
    return t;
}

int main()
{
    {   // merging adds the totals and the per-taxon counts
        Tally a = MakeTally(10, 1000, 6, 5, 4);
        Tally b = MakeTally(20, 3000, 15, 5, 7);
        b.counters[9].incrementReadCount();
        a.Merge(std::move(b));
        CHECK_EQ(a.stats.sequences, 30u);
        CHECK_EQ(a.stats.bases, 4000u);
        CHECK_EQ(a.stats.classified, 21u);
        CHECK_EQ(a.stats.unclassified(), 9u);
        CHECK_EQ(a.counters[5].readCount(), 11u);
        CHECK_EQ(a.counters[9].readCount(), 1u);
    }
    {   // the history can be added to from many threads at once
        History history;
        std::vector<std::thread> threads;
        for (int t = 0; t < 8; ++t)
            threads.emplace_back([&] {
                for (int i = 0; i < 500; ++i)
                    history.Add(MakeTally(2, 100, 1, 3, 1));
            });
        for (auto &t : threads) t.join();
        // all 8 * 500 additions of (2 sequences, 100 bases, 1 classified) are there
        ClassificationStats total = history.Stats();
        CHECK_EQ(total.sequences, 8000u);
        CHECK_EQ(total.bases, 400000u);
        CHECK_EQ(total.classified, 4000u);
        CHECK_EQ(DescribeTotals(total),
                 std::string("8000 sequences (0.40 Mbp) processed.\n"
                             "4000 sequences classified (50.00%).\n"
                             "4000 sequences unclassified (50.00%).\n"));
    }
    {   // the text describing a run
        ClassificationStats s;
        s.sequences = 1000;
        s.bases = 5000000;
        s.classified = 250;
        CHECK_EQ(DescribeRun(s, 30.0),
                 std::string("1000 sequences (5.00 Mbp) processed in 30.00s (2.00 Kseq/m, 10.00 Mbp/m).\n"
                             "\t250 sequences classified (25.00%)\n"
                             "\t750 sequences unclassified (75.00%)\n"));
    }
    return finish("tally_test");
}
