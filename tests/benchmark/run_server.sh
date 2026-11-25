#!/bin/bash

# Benchmark: start a server, send reads from several clients at once, and report the throughput.
#   run_server.sh THREADS PORT CLIENTS [READS.fastq.gz [DB_DIR]]
# e.g.
#   ./run_server.sh 2 8081 4 100times.reads.fastq.gz
#
# Without a database or reads it downloads a small virus database and reads, into
# tests/data (where they are git-ignored). Needs the project built in ../../build/kraken2-server.

# work in tests/data, wherever we were started
here=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$here/../data"
cd "$here/../data"

threads=$1
port=$2
nclients=$3
input=$4
db=$5
echo "threads: ${threads}"
echo "port: ${port}"
echo "nclients: ${nclients}"


PATH=$PATH:$here/../../build/kraken2-server/client:$here/../../build/kraken2-server/server

if [ -z $db ]; then
    if [ ! -d virus-zymo-kraken2 ]; then
        echo " +++ Downloading database +++"
        wget https://ont-exd-int-s3-euwst1-epi2me-labs.s3.amazonaws.com/misc/virus-zymo-kraken.tar.gz
        tar -xzvf virus-zymo-kraken.tar.gz
        rm virus-zymo-kraken.tar.gz
    else
        echo " +++ Database downloaded previously +++"
    fi
    db="virus-zymo-kraken2"
else
    echo " +++ Using provided database +++"
fi

if [ -z "$input" ]; then
    echo ""
    input="virus-zymo-kraken.reads.fastq.gz"
    if [ ! -f "$input" ]; then
        echo " +++ Downloading reads +++"
        wget https://ont-exd-int-s3-euwst1-epi2me-labs.s3.amazonaws.com/misc/$input
    else
        echo " +++ Reads downloaded previously +++"
    fi
else
    echo " +++ Using input file: $input"
fi

echo ""
echo " +++ Starting a client before the server +++"
kraken2_client --port $port --host-ip 127.0.0.1 

echo ""
echo " +++ Starting server +++"
kraken2_server --db $db --host-ip 127.0.0.1 --port $port --wait 2 --thread-pool ${threads} &
sleep 5  # give database time to load

echo ""
echo " +++ Sending reads as stream +++"
function run() {
    input=$1
    port=$2
    client=$3
    echo "$input, $port, $client"
    time kraken2_client --report "kraken.stream.report" --sequence $input  --port $port --host-ip 127.0.0.1 > tmp$client 2> log$client
    #cat tmp$client | sort -k 2 > client-$client.stream.classifications.fasta.txt
    echo "client $3 finished" >> log$client
    echo "client $3 finished"
}
export -f run

printf %s\\n $(seq $nclients) | xargs -n 1 -P $nclients -I {} bash -c "run $input $port {}"

echo ""
echo " +++ Final server stats +++"
kraken2_client --port $port --host-ip 127.0.0.1 | grep sequences 

echo ""
echo " +++ Shutting down server +++ "
kraken2_client --port $port --shutdown

#trap "trap - SIGTERM && kill -- -$$" SIGINT SIGTERM EXIT
