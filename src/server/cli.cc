#include <sysexits.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <iostream>

#include "CLI11.hpp"
#include "server/config.h"

ServerConfig ParseServerConfig(int argc, char **argv)
{
    ServerConfig config;
    bool no_stats = false;
    bool translated_search = false;   // accepted, but see its description

    CLI::App app{"kraken2_server: classify reads sent by kraken2_client, against a "
                 "Kraken 2 database that is loaded once and kept."};
    app.set_help_flag("-h,-H,--help", "Print this help and exit");
    // An option given twice takes its last value, as it always has (scripts rely on
    // being able to override a default they have already put on the command line).
    app.option_defaults()->multi_option_policy(CLI::MultiOptionPolicy::TakeLast);

    // Upper case short options are accepted as well as lower case.
    app.add_option("-d,-D,--db", config.db_path, "Path to the Kraken 2 database")->required();
    app.add_option("-i,-I,--host-ip", config.host, "Server IP address")->capture_default_str();
    app.add_option("-p,-P,--port", config.port, "Port number to listen on")
        ->check(CLI::Range(0, 65535))
        ->capture_default_str();
    app.add_option("-x,-X,--thread-pool", config.threads,
                   "Threads used to classify reads, shared by all clients (0 for one per "
                   "hardware thread)")
        ->check(CLI::Range(0, 4096))
        ->capture_default_str();
    app.add_option("-r,-R,--max-requests", config.max_requests,
                   "Most requests from clients to process at once (0 for gRPC's default)")
        ->check(CLI::NonNegativeNumber)
        ->capture_default_str();
    app.add_flag("-s,-S,--no-stats", no_stats,
                 "Do not keep statistics of everything processed on this server. Saves "
                 "memory long-term.");
    app.add_flag("-o,-O,--memory-mapping", config.memory_mapping,
                 "Map the database rather than loading it into RAM");

    app.add_flag("-k,-K,--report-kmer", config.classify.report_kmer_data,
                 "Include distinct k-mers in reports");
    app.add_flag("-z,-Z,--report-zero", config.report.zero_counts,
                 "Include taxa with a zero count in reports");
    app.add_option("-c,-C,--confidence", config.classify.confidence_threshold,
                   "Confidence score threshold")
        ->check(CLI::Range(0.0, 1.0))
        ->capture_default_str();
    app.add_option("-q,-Q,--min-quality", config.classify.minimum_quality_score,
                   "Minimum base quality used in classification (only effective with FASTQ input)")
        ->check(CLI::Range(0, 100))
        ->capture_default_str();
    app.add_option("-g,-G,--hit-groups", config.classify.minimum_hit_groups,
                   "Minimum number of hit groups (overlapping k-mers sharing the same "
                   "minimizer) needed to make a call")
        ->check(CLI::NonNegativeNumber)
        ->capture_default_str();
    app.add_flag("-t,-T,--translated-search", translated_search,
                 "Has no effect: translated search is used if the database was built for it");
    app.add_option("-w,-W,--wait", config.wait_seconds,
                   "Seconds to wait before loading the database (for testing clients)")
        ->check(CLI::NonNegativeNumber)
        ->capture_default_str();

    try {
        app.parse(argc, argv);
    }
    catch (const CLI::ParseError &e) {
        // help is a success; anything else is a mistake on the command line
        int code = app.exit(e);
        exit(code == 0 ? EX_OK : EX_USAGE);
    }

    config.keep_history = !no_stats;
    config.report.kmer_data = config.classify.report_kmer_data;

    // Fail now, rather than run a server that cannot work, if the database is not there.
    for (const char *name : {"taxo.k2d", "opts.k2d", "hash.k2d"}) {
        std::string file = config.db_path + "/" + name;
        if (access(file.c_str(), R_OK) != 0) {
            std::cerr << "kraken2_server: cannot read " << file << ": " << strerror(errno)
                      << std::endl;
            exit(EX_NOINPUT);
        }
    }
    return config;
}
