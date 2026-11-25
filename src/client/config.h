#pragma once

#include <string>

/// Everything the client is told on its command line.
struct ClientConfig
{
    std::string sequence;     // file to classify; empty to ask for the server's summary instead
    std::string report_file;  // where to write the report, if wanted
    std::string host = "localhost";
    int port = 8080;
    bool shutdown = false;    // ask the server to shut down instead
};

/// Parse the command line. Prints help and exits for --help (status 0), and
/// prints a message and exits with a non-zero status for a bad command line.
ClientConfig ParseClientConfig(int argc, char **argv);
