add_executable(kraken2_server
    main.cc
    cli.cc
    engine.cc
    stream_session.cc
    service.cc)

# SYSTEM: the vendored header is not ours to fix warnings in
target_include_directories(kraken2_server SYSTEM PRIVATE ${PROJECT_SOURCE_DIR}/third_party/CLI11)

target_link_libraries(kraken2_server
    k2s_core
    kraken2_proto # Proto files lib
    ${_PROTOBUF_LIBPROTOBUF}
    ${_GRPC_GRPCPP})
