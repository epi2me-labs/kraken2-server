#!/bin/bash
 set -euo pipefail

export CPLUS_INCLUDE_PATH=${CPLUS_INCLUDE_PATH}:${PREFIX}/include
export LIBRARY_PATH="${PREFIX}/lib"
export LD_LIBRARY_PATH="${PREFIX}/lib"


# TODO: make macOS build work
OS=$(uname)
if [[ "$OS" == "Darwin" ]]; then
    echo "Setting Darwin args"
    export CFLAGS="${CFLAGS} -isysroot ${CONDA_BUILD_SYSROOT} -mmacosx-version-min=${MACOSX_DEPLOYMENT_TARGET}"
fi

# TODO: just use conda packages, needs work in cmake files
echo "Building grpc/proto in: $PWD:"
ls -lh
echo "============================="
export LDFLAGS="-lrt"
export PROTO_DIR=$PWD/proto-build
export PATH="$PROTO_DIR/bin:$PATH"

mkdir -p $PROTO_DIR
git clone --recurse-submodules -b v1.46.3 --depth 1 --shallow-submodules https://github.com/grpc/grpc
mkdir -p grpc/cmake/build
pushd grpc/cmake/build
cmake -DgRPC_INSTALL=ON \
      -DgRPC_BUILD_TESTS=OFF \
      -DCMAKE_INSTALL_PREFIX=$PROTO_DIR \
      ../..
make -j 8
make install
popd



echo "Building kraken2-server in: $PWD:"
ls -lh
echo "============================="
mkdir -p build
pushd build
echo "Running cmake"
cmake -DCMAKE_PREFIX_PATH=${PROTO_DIR} -DCMAKE_BUILD_TYPE=Release ..
make -j 8
 
mkdir -p "$PREFIX/bin"
for bin in server/kraken2_server client/kraken2_client; do
    cp $bin $PREFIX/bin/$(basename $bin)
done

popd
