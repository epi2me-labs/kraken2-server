#!/bin/bash

if [ ! -d virus-zymo-kraken2 ]; then
    echo " +++ Downloading database +++"
    wget https://ont-exd-int-s3-euwst1-epi2me-labs.s3.amazonaws.com/misc/virus-zymo-kraken.tar.gz
    tar -xzvf virus-zymo-kraken.tar.gz
    rm virus-zymo-kraken.tar.gz
else
    echo " +++ Database downloaded previously +++"
fi

echo ""
if [ ! -f virus-zymo-kraken.reads.fastq.gz ]; then
    echo " +++ Downloading reads +++"
    wget https://ont-exd-int-s3-euwst1-epi2me-labs.s3.amazonaws.com/misc/virus-zymo-kraken.reads.fastq.gz
else
    echo " +++ Reads downloaded previously +++"
fi

echo ""
echo " +++ Starting a client before the server +++"
../build/client/kraken2_client --port 8080 --host-ip 127.0.0.1 

echo ""
echo " +++ Starting server +++"
../build/server/kraken2_server --db virus-zymo-kraken2 --host-ip 127.0.0.1 --wait 2 &
sleep 5  # give database time to load

echo ""
echo " +++ Sending reads as stream +++"
../build/client/kraken2_client --report "kraken.stream.report" --sequence virus-zymo-kraken.reads.fastq.gz --port 8080 --host-ip 127.0.0.1 \
    | sort -k 2 > client.stream.classifications.fasta.txt

echo ""
echo " +++ Shutting down server +++ "
../build/client/kraken2_client --port 8080 --shutdown

#trap "trap - SIGTERM && kill -- -$$" SIGINT SIGTERM EXIT
