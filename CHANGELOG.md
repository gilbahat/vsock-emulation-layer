# Changelog

All notable changes to this project are documented here. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project
adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.1.0] - 2026-07-20

Initial public release.

### Added
- `vsock_unix.h` — macOS-safe `AF_VSOCK`/`sockaddr_vm` shim (prefers the SDK's
  `<sys/vsock.h>` when present) plus the `(cid,port)->path` convention helpers.
- `interpose.c` → `libvsock_unix.dylib` — the interposer, usable at runtime via
  `DYLD_INSERT_LIBRARIES` (tier 1) or linked as a dependency (tier 2).
- `vsock-emu` — host peer CLI for being either end of the convention.
- `tests/vsock_echo.c` and the `run_convention.sh` / `run_interposer.sh`
  self-tests (`make test`).
- `make install` / `make uninstall` targets and a Homebrew tap.
- Docs: `README.md`, `docs/DESIGN.md`, `CONTRIBUTING.md`; ISC license.

[Unreleased]: https://github.com/gilbahat/vsock-emulation-layer/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/gilbahat/vsock-emulation-layer/releases/tag/v0.1.0
