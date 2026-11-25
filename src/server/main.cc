#include <sysexits.h>

#include <chrono>
#include <iostream>
#include <memory>

#include <grpcpp/grpcpp.h>

#include "server/config.h"
#include "server/engine.h"
#include "server/service.h"
#include "server/shutdown.h"

/**
 * Serve until a shutdown is requested.
 * @return EX_OK for a clean shutdown, EX_UNAVAILABLE if the server could not be started.
 */
static int Serve(const ServerConfig &config, ClassificationService &service, ShutdownLatch &shutdown)
{
    const std::string address = config.host + ":" + std::to_string(config.port);

    // Sets the max number of concurrent requests
    grpc::ResourceQuota quota;
    if (config.max_requests > 0) {
        // +1 to account for the server thread itself. If one thread were to
        // be specified, all requests are rejected.
        quota.SetMaxThreads(config.max_requests + 1);
    }

    grpc::ServerBuilder builder;
    builder.AddListeningPort(address, grpc::InsecureServerCredentials());
    // don't use the port if it is already in use
    builder.AddChannelArgument(GRPC_ARG_ALLOW_REUSEPORT, 0);
    builder.SetResourceQuota(quota);
    builder.RegisterService(&service);
    // allow 128Mb messages
    const int max_message = 128 * 1024 * 1024;
    builder.SetMaxSendMessageSize(max_message);
    builder.SetMaxMessageSize(max_message);
    builder.SetMaxReceiveMessageSize(max_message);

    std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
    if (!server) {
        std::cerr << "Failed to start server on " << address << ". See above for more details."
                  << std::endl;
        return EX_UNAVAILABLE;
    }
    std::cout << "Server listening on " << address << ". Press Ctrl-C to end." << std::endl;

    shutdown.Wait();
    std::cerr << "Shutting down." << std::endl;
    // Lets in-flight calls finish, but gives up on them after a while so a
    // stalled client cannot keep the server alive indefinitely.
    server->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds(10));
    return EX_OK;
}

int main(int argc, char **argv)
{
    ServerConfig config = ParseServerConfig(argc, argv);

    ShutdownLatch shutdown;
    SignalWatcher signals(shutdown);   // before any other thread starts

    Engine engine(config);
    ClassificationService service(engine, shutdown);

    int status = Serve(config, service, shutdown);
    if (status == EX_OK && engine.state() == IndexState::Failed)
        status = EX_IOERR;
    return status;
}
