# Test data

Data for the benchmark. Nothing in here is in git (see `.gitignore`) except this file,
and nothing in the automated checks needs it: they generate their own small databases
(`tests/integration/make_test_databases.py`).

`tests/benchmark/run_server.sh` downloads what it uses here if it does not find it:

* `virus-zymo-kraken2/`, a small virus database
* `virus-zymo-kraken.reads.fastq.gz`, reads to classify

Note the script only checks that `virus-zymo-kraken2/` exists, not that it is
complete, so an interrupted download needs deleting by hand (a complete database
has `hash.k2d`, `opts.k2d` and `taxo.k2d`).
