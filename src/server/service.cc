#include "server/service.h"

#include <iostream>

#include "server/stream_session.h"

using grpc::Status;
using grpc::StatusCode;
using namespace kraken2proto;

Status ClassificationService::IndexStatus() const
{
    switch (engine_.state()) {
    case IndexState::Ready:
        return Status::OK;
    case IndexState::Failed:
        return Status(StatusCode::FAILED_PRECONDITION,
                      "There was an error loading the index, the server will remain "
                      "unavailable without intervention.");
    case IndexState::Loading:
    default:
        return Status(StatusCode::UNAVAILABLE, "Index not loaded yet, please wait.");
    }
}

Status ClassificationService::GetSummary(grpc::ServerContext *, const Kraken2SummaryRequest *,
                                         Kraken2SummaryResults *results)
{
    Status status = IndexStatus();
    if (!status.ok())
        return status;

    // Only return a summary if the server is recording history, else say so.
    if (engine_.config().keep_history)
        results->set_summary(engine_.history().Summary(engine_.index(), engine_.config().report));
    else
        results->set_summary("Summary not available on this server.");
    return Status::OK;
}

Status ClassificationService::ServerReady(grpc::ServerContext *, const Kraken2ReadyRequest *,
                                          Kraken2ReadyResult *results)
{
    results->set_ready(engine_.state() == IndexState::Ready);
    return IndexStatus();
}

Status ClassificationService::RemoteShutdown(grpc::ServerContext *, const Kraken2ShutdownRequest *,
                                             Kraken2ShutdownResult *result)
{
    std::cerr << "Received shutdown request." << std::endl;
    shutdown_.Request();
    result->set_successful(true);
    return Status::OK;
}

Status ClassificationService::ClassifyStream(grpc::ServerContext *context, ClassifyStreamIO *stream)
{
    Status status = IndexStatus();
    if (!status.ok())
        return status;

    std::string report;
    status = StreamSession(engine_, context, stream).Run(report);
    if (!status.ok())
        return status;

    // If the connection is still open, finish with a message holding the report.
    if (!context->IsCancelled()) {
        Kraken2SequenceStreamResult summary;
        summary.set_summary(report);
        stream->Write(summary);
    }
    return Status::OK;
}
