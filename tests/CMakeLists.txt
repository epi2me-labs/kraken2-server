# Tests that talk to a running server directly over gRPC
add_executable(grpc_error_test grpc_error_test.cc)
target_link_libraries(grpc_error_test
    kraken2_proto
    ${_PROTOBUF_LIBPROTOBUF}
    ${_GRPC_GRPCPP}
    Threads::Threads)
