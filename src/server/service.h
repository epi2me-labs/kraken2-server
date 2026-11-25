#pragma once

#include "Kraken2.grpc.pb.h"
#include "server/engine.h"
#include "server/shutdown.h"
#include "server/stream_session.h"

/// The server's gRPC interface: translates requests into calls on the Engine.
class ClassificationService final : public kraken2proto::Kraken2Service::Service
{
public:
    ClassificationService(Engine &engine, ShutdownLatch &shutdown)
        : engine_(engine), shutdown_(shutdown) {}

    /// The classification history on the server.
    grpc::Status GetSummary(grpc::ServerContext *context,
                            const kraken2proto::Kraken2SummaryRequest *request,
                            kraken2proto::Kraken2SummaryResults *results) override;

    /// Whether the database is loaded. Clients use the status to tell "still
    /// loading" (UNAVAILABLE) from "broken" (FAILED_PRECONDITION).
    grpc::Status ServerReady(grpc::ServerContext *context,
                             const kraken2proto::Kraken2ReadyRequest *request,
                             kraken2proto::Kraken2ReadyResult *results) override;

    /// Ask the server to shut down.
    grpc::Status RemoteShutdown(grpc::ServerContext *context,
                                const kraken2proto::Kraken2ShutdownRequest *request,
                                kraken2proto::Kraken2ShutdownResult *result) override;

    /// Classify a stream of sequences, answering with a stream of classifications
    /// and finally the report.
    grpc::Status ClassifyStream(grpc::ServerContext *context, ClassifyStreamIO *stream) override;

private:
    grpc::Status IndexStatus() const;

    Engine &engine_;
    ShutdownLatch &shutdown_;
};
