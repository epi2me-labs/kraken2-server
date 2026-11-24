# Changelog
All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]
### Added
- Tests: synthetic DNA (32 and 40 bit cell) and protein databases, comparison with
  upstream `classify`, a multi-client check, and a gRPC error-path test
  (`testing/run_all_checks.sh`).
### Changed
- gRPC and protobuf are taken from conda-forge packages in the conda recipe and
  CI, rather than built from source. Builds with libgrpc 1.85.
- Updated to kraken2 commit 8d0e7ad (v2.18.0-RC2). Per-read classifications and
  reports match the upstream `classify` program (see `testing/compare_with_kraken2.sh`).
  Databases with 40-bit hash cells are now supported.
- The database loads in the background, so the server accepts connections
  immediately and tells clients to wait until it is ready.
- Invalid command line options now exit with a non-zero status and a usage message.
  Values are validated strictly. Upper case long options (e.g. `--DB`) are no longer
  accepted; upper case short options still are.
- The server exits non-zero if it cannot start (e.g. port in use) or the database
  cannot be loaded.
- The client no longer sends each read a second time as text (`str_representation`).
- The client polls a loading server every 2s rather than 10s.
### Fixed
- Classification no longer spins a CPU waiting for results; throughput is much higher.
- A read with mismatched sequence and quality lengths failed the stream with
  INVALID_ARGUMENT rather than killing the server.
- `-i`, `-t` and `-w` options were rejected as invalid.
- The server shuts down cleanly on repeated SIGINT/SIGTERM, instead of
  crashing on the second signal.
- Race on the summary returned by the server.
- Client crash when closing its input file, and when the input file did not exist.
  The client now exits with an error if its input cannot be read or is malformed.
### Removed
- Unused utility code and the unused `thread_pool_light.hpp`.

## [v0.1.8]
### Fixed
- Receive size of messages in client raised to accomodate larger requests.

## [v0.1.7]
### Fixed
- Several CLI options did not correctly require a value.

## [v0.1.6]
### Fixed
- Memory mapping CLI option was setting translated search mode not enabling memory mapping!

## [v0.1.5]
### Changed
- Update license file.

## [v0.1.4]
### Fixed
- Explicitly send metadata from server to avoid stall, remove hacky sleep.

## [v0.1.3]
### Added
- Client can read input from stdin.
### Fixed
- Work around a non-understood error.

## [v0.1.2]
### Fixed
- Batching of reads for gRPC stream was incorrect, leading to client errors.

## [v0.1.1]
### Added
- Perform classifications in shared thread pool across clients. Allows speed up of
  processing data from a single client.
### Removed
- The "batch" processing mode of the server and client. This was a stream->unary gRPC,
  that did not achieve any useful functionality beyond the stream->stream gRPC.

## [v0.1.0]
### Fixed
- Memory use of client when processing large input files.

## [v0.0.10]
### Fixed
- Loading of database when user does not provide trailing `/`.

## [v0.0.9]
### Fixed
- Segmentation fault in client when fasta/q comment sections are empty.

## [v0.0.8]
### Added
- --wait option to server fo testing.
### Changed
- client waits if server is active but not ready.

## [v0.0.7]
### Changed
- Server loads kraken2 database asynchronously and will inform clients it is unavailable until loaded.

## [v0.0.6]
### Fixed
- Stop multiple servers running from running on the same port.

## [v0.0.5]
### Added
- Remote shutdown RPC so client can stop server.
- Add IP address option to server.

## [v0.0.4]
### Changed
- kraken2 report data now output to file. 


## [v0.0.3]
First useful release.

### Added
- kraken2_server/client programs.
- conda packaging.
