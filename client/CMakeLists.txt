# Create executable for the server
add_executable(kraken2_client
    kraken2_client.cc)
find_package(ZLIB)

target_include_directories(kraken2_server PUBLIC .)

# Add dependencies / links for this executable
target_link_libraries(kraken2_client
    ZLIB::ZLIB
    kraken2_proto # Proto files lib
    server_client_utils # Additional utils lib
    classify
    ${_PROTOBUF_LIBPROTOBUF}
    ${_REFLECTION}
    ${_GRPC_GRPCPP})
