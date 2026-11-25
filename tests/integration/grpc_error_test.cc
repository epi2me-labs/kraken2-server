// Tests how the server handles bad input, talking to it directly over gRPC
// (the normal client's parser rejects malformed reads before they are sent).
//
// The server must have been started with --min-quality > 0, as that is when
// a sequence/quality length mismatch is detected.
//
//   grpc_error_test HOST PORT
//
// Exits 0 if all checks pass.
#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

#include <grpcpp/grpcpp.h>

#include "Kraken2.grpc.pb.h"

using grpc::ClientContext;
using grpc::Status;
using grpc::StatusCode;
using namespace kraken2proto;
using namespace std::chrono_literals;

static int failures = 0;

#define CHECK(cond, what)                                                  \
    do {                                                                   \
        if (cond) {                                                        \
            std::cout << "  ok    " << what << std::endl;                  \
        } else {                                                           \
            std::cout << "  FAIL  " << what << " (line " << __LINE__       \
                      << ")" << std::endl;                                 \
            failures++;                                                    \
        }                                                                  \
    } while (0)

static void AddRead(Kraken2SequenceRequestMulti &req, const std::string &id,
                    const std::string &seq, const std::string &quals) {
    auto *r = req.add_seqs();
    r->set_format(Kraken2SequenceRequest::FORMAT_FASTQ);
    r->set_id(id);
    r->set_header("@" + id);
    r->set_seq(seq);
    r->set_quals(quals);
}

struct StreamOutcome {
    Status status;
    size_t classifications = 0;
    bool got_summary = false;
};

// Send each request as its own message, then read everything back.
static StreamOutcome RunStream(Kraken2Service::Stub &stub,
                               const std::vector<Kraken2SequenceRequestMulti> &reqs) {
    ClientContext ctx;
    auto stream = stub.ClassifyStream(&ctx);
    // Reading concurrently with writing, as the server may fail the call
    // before we have finished sending.
    StreamOutcome out;
    std::thread reader([&] {
        Kraken2SequenceStreamResult res;
        while (stream->Read(&res)) {
            if (res.has_classifications())
                out.classifications += res.classifications().classes_size();
            if (res.has_summary())
                out.got_summary = true;
        }
    });
    for (const auto &req : reqs) {
        if (!stream->Write(req))
            break;
    }
    stream->WritesDone();
    reader.join();
    out.status = stream->Finish();
    return out;
}

static std::string Bases(size_t n) {
    static const char *unit = "ACGTTGCAAGCTTCGA";
    std::string s;
    while (s.size() < n)
        s += unit;
    s.resize(n);
    return s;
}

static Kraken2SequenceRequestMulti GoodRead(const std::string &id, size_t len = 200) {
    Kraken2SequenceRequestMulti req;
    AddRead(req, id, Bases(len), std::string(len, 'I'));
    return req;
}

static Kraken2SequenceRequestMulti BadRead(const std::string &id) {
    Kraken2SequenceRequestMulti req;
    // quality string is shorter than the sequence
    AddRead(req, id, Bases(200), std::string(4, 'I'));
    return req;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        std::cerr << "usage: grpc_error_test HOST PORT" << std::endl;
        return 64;
    }
    const std::string address = std::string(argv[1]) + ":" + argv[2];
    auto stub = Kraken2Service::NewStub(
        grpc::CreateChannel(address, grpc::InsecureChannelCredentials()));

    // wait for the server to come up and load its database
    bool ready = false;
    for (int i = 0; i < 300 && !ready; ++i) {
        ClientContext ctx;
        Kraken2ReadyRequest req;
        Kraken2ReadyResult res;
        ready = stub->ServerReady(&ctx, req, &res).ok() && res.ready();
        if (!ready)
            std::this_thread::sleep_for(200ms);
    }
    if (!ready) {
        std::cerr << "server did not become ready" << std::endl;
        return 1;
    }

    std::cout << "valid read is classified" << std::endl;
    {
        auto out = RunStream(*stub, {GoodRead("good1")});
        CHECK(out.status.ok(), "stream status is OK");
        CHECK(out.classifications == 1, "one classification returned");
        CHECK(out.got_summary, "report sent at the end");
    }

    std::cout << "read with mismatched sequence/quality lengths" << std::endl;
    {
        auto out = RunStream(*stub, {BadRead("bad1")});
        CHECK(out.status.error_code() == StatusCode::INVALID_ARGUMENT,
              "stream fails with INVALID_ARGUMENT");
        CHECK(out.status.error_message().find("bad1") != std::string::npos,
              "error names the read: '" + out.status.error_message() + "'");
        CHECK(!out.got_summary, "no report sent for a failed stream");
    }

    std::cout << "bad read among many good ones" << std::endl;
    {
        std::vector<Kraken2SequenceRequestMulti> reqs;
        for (int i = 0; i < 50; ++i)
            reqs.push_back(GoodRead("g" + std::to_string(i)));
        reqs.push_back(BadRead("bad2"));
        for (int i = 50; i < 100; ++i)
            reqs.push_back(GoodRead("g" + std::to_string(i)));
        auto out = RunStream(*stub, reqs);
        CHECK(out.status.error_code() == StatusCode::INVALID_ARGUMENT,
              "stream fails with INVALID_ARGUMENT");
    }

    std::cout << "server survives, and an unaffected stream is not disturbed" << std::endl;
    {
        // a long good stream running while another stream fails
        StreamOutcome good;
        std::thread t([&] {
            std::vector<Kraken2SequenceRequestMulti> reqs;
            for (int i = 0; i < 400; ++i)
                reqs.push_back(GoodRead("c" + std::to_string(i)));
            good = RunStream(*stub, reqs);
        });
        std::this_thread::sleep_for(20ms);
        auto bad = RunStream(*stub, {BadRead("bad3")});
        t.join();
        CHECK(bad.status.error_code() == StatusCode::INVALID_ARGUMENT,
              "concurrent bad stream fails");
        CHECK(good.status.ok(), "concurrent good stream is OK");
        CHECK(good.classifications == 400, "concurrent good stream got all 400 results");
        CHECK(good.got_summary, "concurrent good stream got its report");
    }

    std::cout << "server still serves after the failures" << std::endl;
    {
        auto out = RunStream(*stub, {GoodRead("good2")});
        CHECK(out.status.ok() && out.classifications == 1, "valid read still classified");
        ClientContext ctx;
        Kraken2SummaryRequest req;
        Kraken2SummaryResults res;
        CHECK(stub->GetSummary(&ctx, req, &res).ok(), "summary still available");
    }

    std::cout << (failures == 0 ? "PASSED" : "FAILED") << std::endl;
    return failures == 0 ? 0 : 1;
}
