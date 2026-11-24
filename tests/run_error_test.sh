#!/bin/bash
# Start a server and run the gRPC error-path test against it.
#   run_error_test.sh BUILD_DIR DB_DIR
set -euo pipefail
build=$1; db=$2
port=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])')
log=$(mktemp)
"$build/server/kraken2_server" --db "$db" --host-ip 127.0.0.1 --port "$port" \
    --thread-pool 4 --min-quality 5 > "$log" 2>&1 < /dev/null &
pid=$!
trap 'kill $pid 2>/dev/null || true; rm -f "$log"' EXIT
status=0
"$build/tests/grpc_error_test" 127.0.0.1 "$port" || status=$?
# the server must have survived everything the test threw at it
if ! kill -0 "$pid" 2>/dev/null; then
    echo "FAIL: server exited during the test"
    status=1
fi
[ $status = 0 ] || { echo "--- server log"; cat "$log"; }
exit $status
