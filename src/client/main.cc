#include <climits>
#include <iostream>

#include <grpcpp/grpcpp.h>

#include "client.h"
#include "client/config.h"

int main(int argc, char **argv)
{
    ClientConfig config = ParseClientConfig(argc, argv);
    // results are printed a line at a time; don't sync with C stdio for each
    std::ios::sync_with_stdio(false);

    const std::string address = config.host + ":" + std::to_string(config.port);
    std::cerr << "Connecting to server: " << address << "." << std::endl;

    // The client breaks sequence batches up to stay below the 128Mb message size
    // set in the server. It must be able to receive as much in return: the
    // default would be 4MB, so just set it to the maximum.
    grpc::ChannelArguments channel_args;
    channel_args.SetMaxReceiveMessageSize(INT_MAX);
    Client client(grpc::CreateCustomChannel(address, grpc::InsecureChannelCredentials(), channel_args));

    int status;
    if (config.shutdown)
        status = client.Shutdown();
    else if (config.sequence.empty())
        status = client.PrintSummary();
    else
        status = client.Classify(config.sequence, config.report_file);

    std::cout.flush();
    std::cerr << "Return code: " << status << std::endl;
    return status;
}
