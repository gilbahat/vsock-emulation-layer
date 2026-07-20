# Security Policy

## Supported versions

This is a small, single-track project; only the latest release (and `main`)
receive fixes.

| Version | Supported |
| --- | --- |
| latest release | :white_check_mark: |
| older | :x: |

## Reporting a vulnerability

Please report suspected vulnerabilities privately — do **not** open a public
issue for them.

- Preferred: open a [GitHub Security Advisory](https://github.com/gilbahat/vsock-emulation-layer/security/advisories/new).
- Or email **bahat.gil@gmail.com** with details and reproduction steps.

You'll get an acknowledgement, and once a fix is available it will be released
and the reporter credited (unless anonymity is requested).

## Scope note

This tool emulates `AF_VSOCK` over Unix domain sockets for **local development
and testing** on macOS. It intentionally drops CID realism (any `(cid,port)`
is just a filesystem path under `VSOCK_UNIX_DIR`) and provides no isolation or
authentication. It is not a security boundary and should not be used to
sandbox untrusted peers.
