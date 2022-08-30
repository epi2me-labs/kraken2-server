# The minimum permitted CMake version
cmake_minimum_required(VERSION 3.13)

# Project name
project(kraken2_server_client)

# Set the C++ standard to be used
set(CMAKE_CXX_STANDARD 17)

# Locate and verify version of protobuf
set(protobuf_MODULE_COMPATIBLE TRUE)
find_package(Protobuf CONFIG REQUIRED)
message(STATUS "Using protobuf ${Protobuf_VERSION}")

## Set a variable which points to a library installed on machine - Protobuf
set(_PROTOBUF_LIBPROTOBUF protobuf::libprotobuf)

## Set a variable which points to a library installed on machine - gRPC C++ Reflection
set(_REFLECTION gRPC::grpc++_reflection)

## Find and set a var for the exectuable for protoc
if(CMAKE_CROSSCOMPILING)
    find_program(_PROTOBUF_PROTOC protoc)
else()
    set(_PROTOBUF_PROTOC $<TARGET_FILE:protobuf::protoc>)
endif()

# Locate and verify version of gRPC
find_package(gRPC CONFIG REQUIRED)
message(STATUS "Using gRPC ${gRPC_VERSION}")

# Set a variable which points to a library installed on machine - gRPC for C++
set(_GRPC_GRPCPP gRPC::grpc++)

# Find and set a var for the exectuable for gRPC C++ plugin
if(CMAKE_CROSSCOMPILING)
    find_program(_GRPC_CPP_PLUGIN_EXECUTABLE grpc_cpp_plugin)
else()
    set(_GRPC_CPP_PLUGIN_EXECUTABLE $<TARGET_FILE:gRPC::grpc_cpp_plugin>)
endif()

include(FindOpenMP)

if(OPENMP_FOUND)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${OpenMP_CXX_FLAGS}")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${OpenMP_EXE_LINKER_FLAGS}")
else(OPENMP_FOUND)
    message("ERROR: OpenMP could not be found.")
endif(OPENMP_FOUND)

# Sources for file reading and messaging
add_library(classify
    kraken2/src/compact_hash.cc
    kraken2/src/taxonomy.cc
    kraken2/src/mmap_file.cc
    kraken2/src/omp_hack.cc
    kraken2/src/mmscanner.cc
    kraken2/src/seqreader.cc
    kraken2/src/aa_translate.cc
    kraken2/src/utilities.cc
    kraken2/src/hyperloglogplus.cc)

# Specify the headers (include) for this lib (target) and declare them PUBLIC so are findable by other libs/executables
target_include_directories(classify PUBLIC kraken2/src/)

# Execute the CMakeLists.txt in these folders
add_subdirectory(protos)
add_subdirectory(utils)
add_subdirectory(server)
add_subdirectory(client)
