// Tests the Classifier against a real database, by properties rather than exact
// numbers: reads simulated from a genome (named ..._t<taxid>) should be called
// as that taxon or one of its ancestors, random reads should not be called, and
// results must not depend on what was classified before.
//
//   classifier_test DB_DIR READS.fastq
#include <algorithm>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "check.h"
#include "core/classifier.h"

using namespace k2s;

struct FastqRead { std::string id, seq, quals; };

static std::vector<FastqRead> LoadFastq(const std::string &path)
{
    std::ifstream in(path);
    std::vector<FastqRead> reads;
    std::string header, seq, plus, quals;
    while (std::getline(in, header) && std::getline(in, seq) && std::getline(in, plus) &&
           std::getline(in, quals))
        reads.push_back({header.substr(1), seq, quals});
    return reads;
}

static Read View(const FastqRead &r) { return Read{r.id, r.seq, r.quals, true}; }

// "r12_t1423" -> 1423; 0 if the name carries no truth
static uint64_t Truth(const std::string &id)
{
    auto pos = id.rfind("_t");
    return pos == std::string::npos ? 0 : std::stoull(id.substr(pos + 2));
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        std::cerr << "usage: classifier_test DB_DIR READS.fastq" << std::endl;
        return 64;
    }
    auto reads = LoadFastq(argv[2]);
    CHECK(reads.size() > 100);

    Index index(argv[1], false);
    kraken2::Taxonomy &taxonomy = index.taxonomy();
    taxonomy.GenerateExternalToInternalIDMap();
    std::cout << "database is " << (index.translated() ? "protein (translated search)" : "DNA")
              << ", " << reads.size() << " reads" << std::endl;

    ClassifyParams params;
    Classifier classifier(index, params);

    // ---- what is called ----
    size_t truth_reads = 0, truth_called = 0, random_reads = 0, random_called = 0;
    size_t classified = 0, wrong = 0;
    std::vector<Classification> first_pass(reads.size());
    kraken2::taxon_counters_t counters;
    for (size_t i = 0; i < reads.size(); ++i) {
        bool called = classifier.Classify(View(reads[i]), first_pass[i], counters);
        CHECK_EQ(called, first_pass[i].taxon != 0);
        classified += called;
        uint64_t truth = Truth(reads[i].id);
        if (truth) {
            truth_reads++;
            if (called) {
                truth_called++;
                // the call must be the true taxon or one of its ancestors
                if (!taxonomy.IsAAncestorOfB(first_pass[i].taxon, taxonomy.GetInternalID(truth))) {
                    wrong++;
                    if (wrong < 4)
                        std::cerr << "  " << reads[i].id << " called as "
                                  << classifier.ExternalId(first_pass[i].taxon) << std::endl;
                }
            }
        } else if (reads[i].id[0] == 'u' || reads[i].id.rfind("pu", 0) == 0) {
            random_reads++;
            random_called += called;
        }
    }
    std::cout << "classified " << classified << " of " << reads.size() << std::endl;
    CHECK(truth_reads > 0);
    CHECK(truth_called * 10 >= truth_reads * 6);   // most reads from the genomes are called
    CHECK_EQ(wrong, 0u);                           // and never as the wrong lineage
    CHECK(random_reads > 0);
    CHECK(random_called * 20 <= random_reads);     // random sequence is (nearly) never called

    // ---- the counters add up ----
    uint64_t counted = 0;
    for (auto &c : counters) counted += c.second.readCount();
    CHECK_EQ(counted, static_cast<uint64_t>(classified));

    // ---- results do not depend on what was classified before (scratch space is reused) ----
    {
        Classifier fresh(index, params);
        Classification c;
        kraken2::taxon_counters_t scratch_counters;
        // in reverse order, with a classifier that has seen everything already
        for (size_t i = reads.size(); i-- > 0;) {
            classifier.Classify(View(reads[i]), c, scratch_counters);
            CHECK_EQ(c.taxon, first_pass[i].taxon);
            CHECK(c.hit_runs == first_pass[i].hit_runs);
        }
        // and with a brand new one
        for (size_t i = 0; i < reads.size(); i += 7) {
            fresh.Classify(View(reads[i]), c, scratch_counters);
            CHECK_EQ(c.taxon, first_pass[i].taxon);
            CHECK(c.hit_runs == first_pass[i].hit_runs);
        }
    }

    // ---- memory mapping the database does not change anything ----
    {
        Index mapped(argv[1], true);
        Classifier mc(mapped, params);
        Classification c;
        kraken2::taxon_counters_t scratch_counters;
        for (size_t i = 0; i < reads.size(); i += 5) {
            mc.Classify(View(reads[i]), c, scratch_counters);
            CHECK_EQ(c.taxon, first_pass[i].taxon);
        }
    }

    // ---- settings ----
    {
        ClassifyParams strict = params;
        strict.confidence_threshold = 1.0;
        Classifier sc(index, strict);
        Classification c;
        kraken2::taxon_counters_t scratch_counters;
        size_t strict_called = 0;
        for (auto &r : reads) strict_called += sc.Classify(View(r), c, scratch_counters);
        CHECK(strict_called < classified);   // demanding full support calls fewer reads

        ClassifyParams few = params;
        few.minimum_hit_groups = 1000000;
        Classifier fc(index, few);
        size_t few_called = 0;
        for (auto &r : reads) few_called += fc.Classify(View(r), c, scratch_counters);
        CHECK_EQ(few_called, 0u);            // an impossible number of hit groups calls nothing
    }

    // ---- unusual reads ----
    {
        Classification c;
        kraken2::taxon_counters_t scratch_counters;
        std::string tiny = "ACGT";
        CHECK(!classifier.Classify(Read{"tiny", tiny, "", false}, c, scratch_counters));
        std::string hitlist;
        classifier.AppendHitlist(c, hitlist);
        if (index.translated())   // the six reading frames are still marked, as upstream prints them
            CHECK(hitlist.find_first_of("0123456789") == std::string::npos);
        else
            CHECK_EQ(hitlist, std::string("0:0"));   // too short to have any minimizers

        std::string empty;
        CHECK(!classifier.Classify(Read{"empty", empty, "", false}, c, scratch_counters));

        // masking: a mismatched quality string is an error, but only when it is wanted
        ClassifyParams masking = params;
        masking.minimum_quality_score = 10;
        Classifier mc(index, masking);
        std::string bases(100, 'A'), short_quals(4, 'I');
        bool threw = false;
        try {
            mc.Classify(Read{"mismatch", bases, short_quals, true}, c, scratch_counters);
        } catch (const std::invalid_argument &ex) {
            threw = std::string(ex.what()).find("mismatch") != std::string::npos;
        }
        CHECK(threw);
        bool threw_without_masking = false;
        try {
            classifier.Classify(Read{"mismatch", bases, short_quals, true}, c, scratch_counters);
        } catch (const std::exception &) {
            threw_without_masking = true;
        }
        CHECK(!threw_without_masking);   // not an error when quality masking is off
        // FASTA has no qualities, so masking has nothing to do
        mc.Classify(Read{"fasta", bases, "", false}, c, scratch_counters);

        // masking everything leaves nothing to call
        masking.minimum_quality_score = 90;
        Classifier all_masked(index, masking);
        FastqRead good;
        for (auto &r : reads) if (Truth(r.id) && r.seq.size() > 500) { good = r; break; }
        CHECK(!good.seq.empty());
        CHECK(!all_masked.Classify(View(good), c, scratch_counters));
    }

    return finish("classifier_test");
}
