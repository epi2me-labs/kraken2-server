# Locate the .proto file(s) so we can build the gRPC files
get_filename_component(kraken2_proto "Kraken2.proto" ABSOLUTE)
get_filename_component(kraken2_proto_path "${kraken2_proto}" PATH)

# Set variables for the gRPC files that will be generated from the .proto file(s)
set(kraken2_proto_sources "${CMAKE_CURRENT_BINARY_DIR}/Kraken2.pb.cc")
set(kraken2_proto_headers "${CMAKE_CURRENT_BINARY_DIR}/Kraken2.pb.h")
set(kraken2_grpc_sources "${CMAKE_CURRENT_BINARY_DIR}/Kraken2.grpc.pb.cc")
set(kraken2_grpc_headers "${CMAKE_CURRENT_BINARY_DIR}/Kraken2.grpc.pb.h")

#[[ Execute shell commands - generate the gRPC files from the proto file(s)
        Specify the files that will be output
        Shell command to execute - protoc
        Arguments for protoc - where to place outputs for gRPC files and c++ files
        Arguments for protoc - where the protoc file(s) is(are)
        Arguments for protoc - use c++ plugin
        Arguments for protoc - the file at the -I path
        Files on which this commands depends - the proto file(s)
]]
add_custom_command(
    OUTPUT "${kraken2_proto_sources}" "${kraken2_proto_headers}" "${kraken2_grpc_sources}" "${kraken2_grpc_headers}"
    COMMAND ${_PROTOBUF_PROTOC}
    ARGS --grpc_out "${CMAKE_CURRENT_BINARY_DIR}" --cpp_out "${CMAKE_CURRENT_BINARY_DIR}"
    -I "${kraken2_proto_path}"
    --plugin=protoc-gen-grpc="${_GRPC_CPP_PLUGIN_EXECUTABLE}"
    "${kraken2_proto}"
    DEPENDS "${kraken2_proto}")

# Create the Proto library from the generated files
add_library(kraken2_proto
    ${kraken2_proto_sources}
    ${kraken2_proto_headers}
    ${kraken2_grpc_sources}
    ${kraken2_grpc_headers})

# Specify the headers (include) for this lib (target) and declare them PUBLIC so are findable by other libs/executables
target_include_directories(kraken2_proto PUBLIC "${CMAKE_CURRENT_BINARY_DIR}")

# Add dependencies / links for this library
target_link_libraries(kraken2_proto
    ${_PROTOBUF_LIBPROTOBUF}
    ${_REFLECTION}
    ${_GRPC_GRPCPP})

# Anything that includes gRPC's headers (and so abseil's) must be compiled with
# NDEBUG, as abseil is. On Linux abseil's Mutex destructor is inline, and
# without NDEBUG it calls Mutex::Dtor(), which release builds of abseil's shared
# library do not export, so linking fails with undefined references. (macOS is
# unaffected, which is why this only showed up in Linux CI.) Linking this target
# passes the definition on to the server, client, tests and utils. The kraken2
# classification code is deliberately left alone, so its asserts stay enabled as
# upstream builds them.
target_compile_definitions(kraken2_proto PUBLIC NDEBUG)
