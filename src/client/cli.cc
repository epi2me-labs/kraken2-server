#include <sysexits.h>

#include "CLI11.hpp"
#include "client/config.h"

ClientConfig ParseClientConfig(int argc, char **argv)
{
    ClientConfig config;

    CLI::App app{"kraken2_client: classify sequences with a kraken2_server. Leave --sequence "
                 "out to ask the server for its summary of everything it has classified."};
    app.set_help_flag("-h,-H,--help", "Print this help and exit");
    // An option given twice takes its last value, as it always has (scripts rely on
    // being able to override a default they have already put on the command line).
    app.option_defaults()->multi_option_policy(CLI::MultiOptionPolicy::TakeLast);

    // Upper case short options are accepted as well as lower case.
    app.add_option("-s,-S,--sequence", config.sequence,
                   "Path to the sequence file (FASTA or FASTQ, optionally gzip compressed; "
                   "- for standard input)");
    app.add_option("-r,-R,--report", config.report_file, "Path to write the report to");
    app.add_option("-i,-I,--host-ip", config.host, "Server IP address")->capture_default_str();
    app.add_option("-p,-P,--port", config.port, "Server port")
        ->check(CLI::Range(0, 65535))
        ->capture_default_str();
    app.add_flag("-k,-K,--shutdown", config.shutdown, "Shut down the server");

    try {
        app.parse(argc, argv);
    }
    catch (const CLI::ParseError &e) {
        int code = app.exit(e);
        exit(code == 0 ? EX_OK : EX_USAGE);
    }
    return config;
}
