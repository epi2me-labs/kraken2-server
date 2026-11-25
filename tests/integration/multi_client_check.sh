#!/bin/bash
# Run several clients at once against one server, each with different reads,
# and check that every client gets exactly the answers upstream `classify`
# gives for its own reads (no cross-talk between streams), and that the
# server's accumulated summary equals classify on all the reads together.
#
# Usage:
#   multi_client_check.sh BUILD_DIR CLASSIFY_BIN DB_DIR READS.fastq[.gz] [NCLIENTS=8] [REPEAT=10]
#
# REPEAT concatenates the reads that many times so the clients overlap for long
# enough to matter.
set -euo pipefail
if [ $# -lt 4 ]; then
    sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'; exit 64
fi
build=$1; classify=$2; db=$3; reads=$4; nclients=${5:-8}; repeat=${6:-10}

work=$(mktemp -d)
server_pid=""
cleanup() { [ -n "$server_pid" ] && kill "$server_pid" 2>/dev/null || true; rm -rf "$work"; }
trap cleanup EXIT

case $reads in *.gz) gzip -dc "$reads" > "$work/one.fq" ;; *) cp "$reads" "$work/one.fq" ;; esac
for _ in $(seq "$repeat"); do cat "$work/one.fq"; done > "$work/all.fq"

# split into per-client files, on read (4 line) boundaries
total_reads=$(( $(wc -l < "$work/all.fq") / 4 ))
per_client=$(( (total_reads + nclients - 1) / nclients ))
split -a 3 -l $(( per_client * 4 )) "$work/all.fq" "$work/part."
parts=("$work"/part.*)
echo "$total_reads reads in ${#parts[@]} clients (~$per_client reads each)"

port=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])')
"$build/server/kraken2_server" --db "$db" --host-ip 127.0.0.1 --port "$port" \
    --thread-pool 4 > "$work/server.log" 2>&1 < /dev/null &
server_pid=$!
# The client waits for as long as it takes for a server to appear, so first make
# sure there is going to be one: fail now, with its log, if the server died.
for _ in $(seq 150); do
    if ! kill -0 "$server_pid" 2>/dev/null; then
        echo "FAIL: the server exited at startup:"; cat "$work/server.log"; server_pid=""; exit 1
    fi
    (exec 3<>"/dev/tcp/127.0.0.1/$port") 2>/dev/null && break
    sleep 0.2
done
# let the server finish loading so that all clients start together
"$build/client/kraken2_client" --host-ip 127.0.0.1 --port "$port" > /dev/null 2>&1 || true

# all clients at once
pids=()
for p in "${parts[@]}"; do
    "$build/client/kraken2_client" --host-ip 127.0.0.1 --port "$port" --sequence "$p" \
        --report "$p.srv.report" > "$p.srv.out" 2> "$p.srv.log" &
    pids+=($!)
done
status=0
for i in "${!pids[@]}"; do
    wait "${pids[$i]}" || { echo "FAIL: client $i exited non-zero"; status=1; }
done

# what the server says it has seen in total
"$build/client/kraken2_client" --host-ip 127.0.0.1 --port "$port" > "$work/summary.txt" 2>/dev/null

"$build/client/kraken2_client" --host-ip 127.0.0.1 --port "$port" --shutdown > /dev/null 2>&1 || true
wait "$server_pid" 2>/dev/null || true; server_pid=""

# the point of the test is overlap: how many streams were in progress at once?
peak=$(awk '/Starting stream handler/ {n++; if (n>m) m=n} /Finished stream handler|Stream failed/ {n--} END {print m+0}' "$work/server.log")
echo "peak concurrent streams on the server: $peak"
if [ "$peak" -lt 2 ]; then
    echo "FAIL: streams did not overlap, so this proved nothing (use a larger REPEAT)"; status=1
fi

# each client against classify on exactly its reads
for p in "${parts[@]}"; do
    name=$(basename "$p")
    "$classify" -H "$db/hash.k2d" -t "$db/taxo.k2d" -o "$db/opts.k2d" -g 2 -M \
        -O "$p.ref.out" -R "$p.ref.report" "$p" 2> /dev/null
    if [ "$(wc -l < "$p.srv.out")" != "$(wc -l < "$p.ref.out")" ]; then
        echo "FAIL: $name: got $(wc -l < "$p.srv.out") results, expected $(wc -l < "$p.ref.out")"; status=1
    elif ! diff -q <(sort "$p.srv.out") <(sort "$p.ref.out") > /dev/null; then
        echo "FAIL: $name: per-read calls differ"
        diff <(sort "$p.srv.out") <(sort "$p.ref.out") | head -3; status=1
    fi
    if ! diff -q <(sed 1d "$p.srv.report") "$p.ref.report" > /dev/null; then
        echo "FAIL: $name: report differs"; status=1
    fi
done

# the server's running total against classify on everything
"$classify" -H "$db/hash.k2d" -t "$db/taxo.k2d" -o "$db/opts.k2d" -g 2 -M \
    -O /dev/null -R "$work/all.ref.report" "$work/all.fq" 2> /dev/null
# summary = header, report rows, blank line, then totals
awk 'NR>1 && /^$/ {exit} NR>1 {print}' "$work/summary.txt" > "$work/summary.report"
if ! diff -q "$work/summary.report" "$work/all.ref.report" > /dev/null; then
    echo "FAIL: server summary differs from classify over all reads"
    diff "$work/summary.report" "$work/all.ref.report" | head -5; status=1
fi
if ! grep -q "^$total_reads sequences" "$work/summary.txt"; then
    echo "FAIL: summary does not account for all $total_reads reads:"; grep sequences "$work/summary.txt" | head -2; status=1
fi

[ $status = 0 ] && echo "OK: ${#parts[@]} concurrent clients each identical to classify; server totals identical over $total_reads reads"
exit $status
