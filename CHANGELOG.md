# Changelog
All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

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
