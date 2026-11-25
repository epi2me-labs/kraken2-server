###
# There are hints below to look for gRPC and Protobuf libraries in the
# build/proto directory as per the instruction in the README.md file.

# The minimum permitted CMake version
cmake_minimum_required(VERSION 3.13)

# Project name
project(kraken2_server_client)

# Optimised unless asked otherwise. Without a build type the compiler gets no
# optimisation flags at all, and the server runs several times slower, which is
# an easy mistake to make by leaving the option off.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Type of build (Release unless set)" FORCE)
endif()

# Set the C++ standard to be used
set(CMAKE_CXX_STANDARD 17)

add_compile_options(-Wall)
add_definitions(-DLINEAR_PROBING)
set(CMAKE_CXX_FLAGS_DEBUG "-g")
set(CMAKE_CXX_FLAGS_RELEASE "-O3")

# Protobuf 22 and later needs Abseil and utf8_range, and their packages must be
# found before protobuf's own is read, or its imported targets cannot be resolved.
# (A gRPC built from source puts them in build/proto; with conda one
# CMAKE_PREFIX_PATH covers all of it. utf8_range only exists with newer protobuf.)
find_package(absl CONFIG REQUIRED PATHS "${CMAKE_CURRENT_SOURCE_DIR}/build/proto/lib/cmake/absl")
find_package(utf8_range CONFIG QUIET HINTS "${CMAKE_CURRENT_SOURCE_DIR}/build/proto")

# Locate and verify version of protobuf
set(protobuf_MODULE_COMPATIBLE TRUE)
find_package(Protobuf CONFIG REQUIRED
            HINTS "${CMAKE_CURRENT_SOURCE_DIR}/build/proto")
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
find_package(gRPC CONFIG REQUIRED
            HINTS "${CMAKE_CURRENT_SOURCE_DIR}/build/proto" )
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

if(OPENMP_FOUND AND NOT NO_OMP)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${OpenMP_CXX_FLAGS}")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${OpenMP_EXE_LINKER_FLAGS}")
else()
    message("ERROR: OpenMP could not be found.")
endif()

find_package(Threads REQUIRED)
find_package(ZLIB REQUIRED)

# Sources for file reading and messaging
add_library(kraken2_upstream STATIC
    third_party/kraken2/src/compact_hash.cc
    third_party/kraken2/src/taxonomy.cc
    third_party/kraken2/src/mmap_file.cc
    third_party/kraken2/src/omp_hack.cc
    third_party/kraken2/src/mmscanner.cc
    third_party/kraken2/src/fast_reader.cc
    third_party/kraken2/src/aa_translate.cc
    third_party/kraken2/src/utilities.cc
    third_party/kraken2/src/hyperloglogplus.cc
    third_party/kraken2/src/reports.cc)

# Specify the headers (include) for this lib (target) and declare them PUBLIC so are findable by other libs/executables
target_include_directories(kraken2_upstream PUBLIC third_party/kraken2/src/)
target_link_libraries(kraken2_upstream PUBLIC ZLIB::ZLIB Threads::Threads)

# Execute the CMakeLists.txt in these folders
enable_testing()

# Our code is under src/. Each part is built in a directory of its own name, so
# configured with -B build/kraken2-server the server ends up at
# build/kraken2-server/server/kraken2_server.
add_subdirectory(src/common common)
add_subdirectory(src/core core)
add_subdirectory(src/protos protos)
add_subdirectory(src/server server)
add_subdirectory(src/client client)
add_subdirectory(tests tests)

