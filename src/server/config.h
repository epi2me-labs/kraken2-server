#pragma once

#include <string>

#include "core/classifier.h"
#include "core/tally.h"

/// Everything the server is told on its command line.
struct ServerConfig
{
    std::string db_path;
    std::string host = "localhost";
    int port = 8080;
    /// Most requests handled at once; 0 leaves it to gRPC.
    int max_requests = 0;
    /// Threads classifying reads, shared by all clients; 0 means one per hardware thread.
    int threads = 1;
    /// Keep running totals of everything classified, for the summary.
    bool keep_history = true;
    bool memory_mapping = false;
    /// Seconds to wait before loading the database (to test clients that wait for the server)
    int wait_seconds = 0;

    k2s::ClassifyParams classify;
    k2s::ReportOptions report;
};

/// Parse the command line. Prints help and exits for --help (status 0), and
/// prints a message and exits with a non-zero status for a bad command line or
/// a database that cannot be read.
ServerConfig ParseServerConfig(int argc, char **argv);
