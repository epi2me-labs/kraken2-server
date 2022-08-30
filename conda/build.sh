#!/bin/bash

export CPLUS_INCLUDE_PATH=${CPLUS_INCLUDE_PATH}:${PREFIX}/include
export LIBRARY_PATH="${PREFIX}/lib"
export LD_LIBRARY_PATH="${PREFIX}/lib"


# TODO: make macOS build work
OS=$(uname)
if [[ "$OS" == "Darwin" ]]; then
    echo "Setting Darwin args"
    export CFLAGS="${CFLAGS} -isysroot ${CONDA_BUILD_SYSROOT} -mmacosx-version-min=${MACOSX_DEPLOYMENT_TARGET}"
fi


echo "Building kraken2-server in: $PWD:"
ls -lh
echo "============================="
mkdir -p build-server
pushd build-server
echo "Running cmake"
cmake ..
make -j 8
 
mkdir -p "$PREFIX/bin"
for bin in server/kraken2_server client/kraken2_client; do
    cp $bin $PREFIX/bin/$(basename $bin)
done

popd
