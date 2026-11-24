#!/bin/bash
# Check that kraken2_server gives the same answers as upstream kraken2's
# `classify` binary: per-read calls and the report body must be identical
# (read order is not preserved by the server, so output is sorted by read ID).
#
# Usage:
#   compare_with_kraken2.sh BUILD_DIR CLASSIFY_BIN DB_DIR READS.fastq[.gz] [-- server options]
#
# BUILD_DIR     cmake build directory holding server/kraken2_server and client/kraken2_client
# CLASSIFY_BIN  `classify` built from the kraken2 submodule
# DB_DIR        directory with hash.k2d, taxo.k2d, opts.k2d
#
# Server options (after `--`) are also translated for classify, so that both
# sides run with the same settings. Supported: --confidence N, --hit-groups N,
# --report-kmer. Default is `--hit-groups 2` to match the server's default.
set -euo pipefail

if [ $# -lt 4 ]; then
    sed -n '2,16p' "$0" | sed 's/^# \{0,1\}//'
    exit 64
fi
build=$1; classify=$2; db=$3; reads=$4
shift 4
[ "${1:-}" = "--" ] && shift

server_opts=("$@")
classify_opts=()
have_hit_groups=0
while [ $# -gt 0 ]; do
    case $1 in
        --confidence) classify_opts+=(-T "$2"); shift ;;
        --hit-groups) classify_opts+=(-g "$2"); have_hit_groups=1; shift ;;
        --report-kmer) classify_opts+=(-K) ;;
        *) echo "unsupported option for comparison: $1" >&2; exit 64 ;;
    esac
    shift
done
if [ $have_hit_groups = 0 ]; then
    classify_opts+=(-g 2)
fi

work=$(mktemp -d)
server_pid=""
cleanup() {
    [ -n "$server_pid" ] && kill "$server_pid" 2>/dev/null || true
    rm -rf "$work"
}
trap cleanup EXIT

case $reads in
    *.gz) gzip -dc "$reads" > "$work/reads.fq" ;;
    *) cp "$reads" "$work/reads.fq" ;;
esac

# a free port
port=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])')

"$build/server/kraken2_server" --db "$db" --host-ip 127.0.0.1 --port "$port" \
    --thread-pool 4 ${server_opts[@]+"${server_opts[@]}"} > "$work/server.log" 2>&1 < /dev/null &
server_pid=$!

# the client waits for the server to report ready
"$build/client/kraken2_client" --host-ip 127.0.0.1 --port "$port" \
    --sequence "$work/reads.fq" --report "$work/server.report" \
    > "$work/server.out" 2> "$work/client.log"

"$build/client/kraken2_client" --host-ip 127.0.0.1 --port "$port" --shutdown > /dev/null 2>&1 || true
wait "$server_pid" 2>/dev/null || true
server_pid=""

"$classify" -H "$db/hash.k2d" -t "$db/taxo.k2d" -o "$db/opts.k2d" \
    -O "$work/classify.out" -R "$work/classify.report" -M \
    ${classify_opts[@]+"${classify_opts[@]}"} "$work/reads.fq" 2> "$work/classify.log"

n_srv=$(wc -l < "$work/server.out")
n_ref=$(wc -l < "$work/classify.out")
status=0
if [ "$n_srv" != "$n_ref" ]; then
    echo "FAIL: server returned $n_srv reads, classify $n_ref"
    status=1
fi
sort -k2,2 "$work/server.out" > "$work/a"
# the server's per-read lines have 5 columns (no taxon name column), classify's too
sort -k2,2 "$work/classify.out" > "$work/b"
if ! diff -q "$work/a" "$work/b" > /dev/null; then
    echo "FAIL: per-read calls differ ($(diff "$work/a" "$work/b" | grep -c '^<') lines)"
    diff "$work/a" "$work/b" | head -5
    status=1
fi
# the server's report starts with a header line that classify's lacks
if ! diff -q <(sed 1d "$work/server.report") "$work/classify.report" > /dev/null; then
    echo "FAIL: report differs"
    diff <(sed 1d "$work/server.report") "$work/classify.report" | head -5
    status=1
fi
if [ $status = 0 ]; then
    echo "OK: $n_srv reads identical; $(grep -c . "$work/classify.report") report lines identical"
fi
exit $status
