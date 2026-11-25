# Kraken2 Server

Kraken2 is a taxonomic sequence classification system. This project builds
on the classification functionality to provide a server-client architecture
to allow two use cases:

* Access to the classification algorithms in low-resource settings by
  sending requests to remote, more powerful servers
* One-time loading of databases (a slow step compared to classification)
  into a persistent process, with subsequent independent classification
  requests as data becomes available

The software is currently a public beta release.

## Installation

The kraken2 server and client are available through our conda channel:

```
mamba create -n kraken2 -c conda-forge -c nanoporetech kraken2-server
```

## Usage

To start a server run:

```
kraken2_server --db <db_path>
```

where `<db_path>` is a directory containing a standard kraken2 database. The
server will wait for requests for clients and respond as necessary.

To classify reads run a client with:

```
kraken2_client --port 8080 --sequence <reads.fq.gz>
```

where `<reads.fq.gz>` can be FASTQ or FASTA either plain text or gzip compressed.

The output gives some of the same details as running the standard
`kraken2` program. Currently it is not identical; the intention is to
in future provide compatible output.


## Building from source

The project can be built with `cmake` >3.13 and a C++17 compiler. The
`third_party/kraken2` directory is a git submodule, so clone with `--recurse-submodules`.

The server-client architecture uses gRPC and protobuf to communicate. The
easiest way to get them, and a compiler, is from conda-forge:

```
mamba create -n kraken2-server-build -c conda-forge \
    libgrpc libprotobuf zlib cmake cxx-compiler make llvm-openmp
mamba activate kraken2-server-build

git clone --recurse-submodules https://github.com/epi2me-labs/kraken2-server.git
cd kraken2-server
cmake -S . -B build/kraken2-server -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=${CONDA_PREFIX}
cmake --build build/kraken2-server -j 8
```

(`llvm-openmp` is needed on macOS; on Linux the compiler provides OpenMP.)
This has been built with libgrpc 1.85 and libprotobuf 35.1 on macOS arm64.

Alternatively, build gRPC yourself (see
[gRPC Dependencies for C++](https://grpc.io/docs/languages/cpp/quickstart/))
into `build/proto`, which the project looks in:

```
# this flag might be needed on some platforms
export LDFLAGS="-lrt"

git clone --recurse-submodules -b v1.85.4 --depth 1 --shallow-submodules https://github.com/grpc/grpc third_party/grpc

# gRPC is built in build/grpc and installed to build/proto
cmake -S third_party/grpc -B build/grpc \
      -DgRPC_INSTALL=ON \
      -DgRPC_BUILD_TESTS=OFF \
      -DgRPC_ZLIB_PROVIDER=package \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX=$PWD/build/proto
cmake --build build/grpc -j 8
cmake --build build/grpc --target install

# the project's CMake looks for gRPC in build/proto by itself
cmake -S . -B build/kraken2-server -DCMAKE_BUILD_TYPE=Release
cmake --build build/kraken2-server -j 8
```

Do not point `CMAKE_PREFIX_PATH` at `build/proto`: a gRPC built without
`-DgRPC_ZLIB_PROVIDER=package` installs its own copy of zlib there, which on macOS
cannot be found at run time, and CMake would pick it over the system zlib.

The server and client executables will be written to:

```
build/kraken2-server/server/kraken2_server
build/kraken2-server/client/kraken2_client
```

## How the code is organised

```
src/
  core/      Classification, with no gRPC in it. This is the part to reuse.
             Index         a loaded database, shareable between threads
             Classifier    classify(read) -> result; one per thread
             Tally/History statistics and per-taxon counts, mergeable
             upstream_port the copy of kraken2's classification algorithm
  server/    The gRPC server: command line, Engine (what clients share),
             StreamSession (one client's stream) and the service itself
  client/    The command line client
  common/    BlockingQueue, and WorkerPool (threads over a bounded queue, so
             submitting waits when it is full): generic, no kraken2 in them
  protos/    The gRPC interface
tests/
  unit/        Unit tests of the core and of common
  integration/ Checks against upstream kraken2 and of the server's error
               handling (see Testing)
  benchmark/   A throughput benchmark script
  data/        Data for trying things out by hand (not in git)
third_party/
  kraken2/     Upstream kraken2, as a git submodule
  CLI11/       The command line parser
  kseq/        FASTA/FASTQ reading, used by the client
  grpc/        gRPC's source, only if you build it yourself (not in git)
conda/         The conda recipe
build/         Everything that is built (not in git)
  kraken2-server/  this project: server/kraken2_server, client/kraken2_client, tests
  kraken2/         upstream kraken2's classify and build_db, for the tests
  proto/           gRPC and protobuf, if you built them yourself
  grpc/            gRPC's own build tree
```

kraken2's own `classify` program keeps its classification code private to
`classify.cc`, so it cannot be linked. `core/upstream_port.{h,cc}` therefore
holds a copy of the few functions involved, labelled with the upstream commit they
came from. It is the only code that has to change when kraken2 changes how it
classifies, and the checks under Testing confirm the results still match.

## Testing

`tests/integration/run_all_checks.sh` runs every check. It needs the `classify` and
`build_db` programs from the `kraken2` submodule (built with its own
`CMakeLists.txt`):

```
cmake -S third_party/kraken2 -B build/kraken2 -DCMAKE_BUILD_TYPE=Release
cmake --build build/kraken2 -j 8 --target classify build_db

tests/integration/run_all_checks.sh build/kraken2-server build/kraken2/src/classify build/kraken2/src/build_db
```

It generates small synthetic databases (DNA with 32 and 40 bit hash cells, and
protein for translated search) and then:

* runs the unit tests (`ctest`), which need nothing running: the queue, the
  worker pool and the tallies;
* runs `tests/unit/classifier_test` on each database: reads simulated from a
  genome are called as that taxon or an ancestor of it, random reads are not
  called, results do not depend on what was classified before, and memory
  mapping the database changes nothing;

* checks that the server returns the same per-read calls and report as
  upstream `classify`, with the same options on both sides
  (`tests/integration/compare_with_kraken2.sh`, which can also be run by itself
  on any database and reads);
* runs several clients at once and checks that each gets exactly the answers for
  its own reads, and that the server's running totals are right
  (`tests/integration/multi_client_check.sh`);
* checks that a malformed read fails only its own stream with a clear error and
  does not harm the server or other streams (`tests/integration/run_error_test.sh`).

## Benchmarks

Benchmarking script from `tests/benchmark/run_server.sh`

**Single client test**

*MacBook Pro 14-inch 2021, M1 Max, 64Gb. macOS 13.2.1. Clang 13.1.6. 1190.33 Mbp per client*

| clients | server threads | client time / s | server throughput / Mbp/m |
|---------|----------------|-----------------|---------------------------|
|       1 |              2 |            48.8 |                      1462 |
|       1 |              4 |            25.1 |                      2839 |
|       1 |              6 |            18.4 |                      3876 |
|       1 |              8 |            16.8 |                      4237 |

**Multi client test**

*Intel Xeon Gold 6230, Ubuntu 16.04.7, gcc 11.3.0. 991.94 Mbp per client*

| clients | server threads | client time / s | server throughput / Mbp/m |
|---------|----------------|-----------------|---------------------------|
|       1 |             64 |            19.8 |                      3011 |
|       2 |             64 |            21.6 |                      5491 |
|       4 |             64 |            24.1 |                      9802 |
|       8 |             64 |            28.9 |                     16335 |
|      16 |             64 |            62.2 |                     14958 |

