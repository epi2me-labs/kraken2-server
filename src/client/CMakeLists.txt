add_executable(kraken2_client
    main.cc
    cli.cc
    client.cc
    sequence_reader.cc)

# SYSTEM: the vendored header is not ours to fix warnings in
target_include_directories(kraken2_client SYSTEM PRIVATE
    ${PROJECT_SOURCE_DIR}/third_party/CLI11
    ${PROJECT_SOURCE_DIR}/third_party/kseq)

target_link_libraries(kraken2_client
    k2s_common
    ZLIB::ZLIB
    kraken2_proto # Proto files lib
    ${_PROTOBUF_LIBPROTOBUF}
    ${_GRPC_GRPCPP})
