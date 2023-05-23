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

The project can be built with `cmake` >3.13 and a C++17 compiler.

The server-client architecture uses gRPC and protobuf to communicate. An
installation of gRPC (with protobuf is required).

The following should be sufficient to setup an installation of gRPC
and protobuf (see [gRPC Dependencies for C++](https://grpc.io/docs/languages/cpp/quickstart/)):

```
INSTALL_ROOT=$PWD  # or something else

# this flag might be needed on some platforms
export LDFLAGS="-lrt"


export PROTO_DIR=$INSTALL_ROOT/proto-build
export PATH="$PROTO_DIR/bin:$PATH"

mkdir -p $PROTO_DIR
git clone --recurse-submodules -b v1.46.3 --depth 1 --shallow-submodules https://github.com/grpc/grpc
mkdir -p grpc/cmake/build
pushd grpc/cmake/build
cmake -DgRPC_INSTALL=ON \
      -DgRPC_BUILD_TESTS=OFF \
      -DCMAKE_INSTALL_PREFIX=$PROTO_DIR \
      ../..
make -j
make install
popd
```

To build the project itself then run:

```
git clone https://github.com/epi2me-labs/kraken2-server.git`
cd kraken2-server
mkdir build
pushd build
# PROTO_DIR as above
cmake -DCMAKE_PREFIX_PATH=${PROTO_DIR} --config Release ..
make -j 8
popd
```

The server and client executables will be written to:

```
build/server/kraken2_server
build/client/kraken2_client
```

## Benchmarks

Benchmarking script from `testing/run_server.sh`

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

