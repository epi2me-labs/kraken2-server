#!/bin/bash
set -eo pipefail

# gRPC and protobuf come from conda packages (see meta.yaml); CMake finds them
# through the host prefix.
mkdir -p build
pushd build
cmake ${CMAKE_ARGS} -DCMAKE_PREFIX_PATH="${PREFIX}" -DCMAKE_BUILD_TYPE=Release ..
make -j "${CPU_COUNT:-4}"

mkdir -p "$PREFIX/bin"
for bin in server/kraken2_server client/kraken2_client; do
    cp $bin $PREFIX/bin/$(basename $bin)
done
popd
