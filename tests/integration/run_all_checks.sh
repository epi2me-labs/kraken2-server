#!/bin/bash
# Run every check: the unit tests; then generates small synthetic databases and
# tests the classifier on each, compares the server with upstream classify on DNA
# (32 and 40 bit cell) and protein (translated search) databases, runs several
# clients concurrently, and runs the gRPC error-path test.
#
#   run_all_checks.sh BUILD_DIR CLASSIFY_BIN BUILD_DB_BIN
#
# BUILD_DIR     cmake build directory of this project
# CLASSIFY_BIN  `classify` from the kraken2 submodule build
# BUILD_DB_BIN  `build_db` from the same build
set -uo pipefail
if [ $# -ne 3 ]; then sed -n '2,11p' "$0" | sed 's/^# \{0,1\}//'; exit 64; fi
build=$1; classify=$2; build_db=$3
here=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

echo "== generating test databases"
python3 -I "$here/make_test_databases.py" "$build_db" "$work" || { echo "FAILED to build test databases"; exit 1; }

failed=()
check() {  # name, command...
    local name=$1; shift
    echo; echo "== $name"
    if "$@"; then :; else failed+=("$name"); fi
}

check "unit tests" bash -c "cd '$build' && ctest --output-on-failure"

for spec in "dna32 dna.fastq" "dna40 dna.fastq" "protein protein.fastq"; do
    read -r db reads <<< "$spec"
    check "$db: classifier" "$build/tests/classifier_test" "$work/$db" "$work/$reads"
    check "$db: defaults" \
        "$here/compare_with_kraken2.sh" "$build" "$classify" "$work/$db" "$work/$reads"
    check "$db: confidence, hit groups, k-mer report" \
        "$here/compare_with_kraken2.sh" "$build" "$classify" "$work/$db" "$work/$reads" \
        -- --confidence 0.05 --hit-groups 2 --report-kmer
    check "$db: min quality, zero counts, memory mapping, other server options" \
        "$here/compare_with_kraken2.sh" "$build" "$classify" "$work/$db" "$work/$reads" \
        -- --min-quality 15 --report-zero --memory-mapping --no-stats --thread-pool 2 --max-requests 4
done
check "8 concurrent clients" \
    "$here/multi_client_check.sh" "$build" "$classify" "$work/dna32" "$work/dna.fastq" 8 10
check "gRPC error handling" \
    "$here/run_error_test.sh" "$build" "$work/dna32"

echo
if [ ${#failed[@]} -eq 0 ]; then
    echo "ALL CHECKS PASSED"; exit 0
fi
echo "FAILED: ${failed[*]}"; exit 1
