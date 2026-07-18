# Design: AF_VSOCK over Unix domain sockets

This document specifies the convention and the translation rules precisely
enough to reimplement in any language, so independent implementations
interoperate.

## Why

`AF_VSOCK` is Linux-only. macOS ships the `<sys/vsock.h>` ABI types (macOS 26+)
but `socket(AF_VSOCK, …)` fails with *"Operation not supported by device"* —
there is no usable vsock. That blocks local development and testing of software
that communicates over vsock (VM/enclave tooling, unikernels, brokers). This
project backs vsock with Unix domain sockets so it "just works" on macOS,
without changing the vsock software itself.

## The convention

A vsock endpoint `(cid, port)` maps to exactly one filesystem path:

```
<base>/<cid>.<port>
```

- `<base>` = `$VSOCK_UNIX_DIR` if set and non-empty; otherwise
  `${TMPDIR:-/tmp}/vsock-unix`. A trailing slash on `TMPDIR` is collapsed (macOS
  `TMPDIR` commonly ends in `/`).
- `<cid>` and `<port>` are unsigned 32-bit integers formatted as decimal.
- The base directory is created (`mkdir -p`, mode `0700`) on bind.

Every party — a listener, a connector, an interposed program, a freestanding
guest — that forms the same `(cid, port)` into the same path meets on the same
socket. That single rule is the entire interop contract.

### CID canonicalization

`(cid, port)` is only a path key; CID realism is intentionally dropped. One
canonicalization applies before forming the path:

- `VMADDR_CID_ANY` (`0xFFFFFFFF`) → `VMADDR_CID_HOST` (`2`).

Rationale: on real vsock a server binds `VMADDR_CID_ANY` (wildcard) and a client
dials the host CID (`2`). With no wildcard on the filesystem, both are pinned to
`2` so they rendezvous. All other CIDs pass through unchanged.

`VMADDR_PORT_ANY` (auto-assigned ports) is not emulated; endpoints use explicit
ports.

### Path length

The path must fit macOS's 104-byte `sun_path`. `"/tmp/vsock-unix/"` plus two
decimal `uint32`s is ~37 bytes; a custom `$VSOCK_UNIX_DIR` must leave room.

## Address translation

Linux `struct sockaddr_vm` and the backing `struct sockaddr_un`:

| vsock field | source | use |
| --- | --- | --- |
| `svm_cid`  | `sockaddr_vm` | canonicalize, format into path |
| `svm_port` | `sockaddr_vm` | format into path |
| `svm_family` | — | ignored (the backing socket is `AF_UNIX`) |

The backing socket is always `AF_UNIX` / `SOCK_STREAM`. On bind, remove any
stale socket file at the path first (`unlink`) so a re-run rebinds cleanly.

Note on the macOS `sockaddr_vm` layout: it is BSD-packed (`svm_len`,
`svm_family` are one byte each) rather than Linux's 2-byte family, but
`svm_port` and `svm_cid` land at the same byte offsets (4 and 8), so reading
those two fields is ABI-compatible with Linux either way.

## Interception surface (interposer / library mode)

To back unmodified `AF_VSOCK` code, intercept these calls and act only when the
fd is an `AF_VSOCK` socket the interceptor created:

| call | action |
| --- | --- |
| `socket(AF_VSOCK, …)` | create `AF_UNIX`/`SOCK_STREAM`; record the fd |
| `bind(fd, sockaddr_vm)` | form path from `(cid,port)`; `mkdir -p`; `unlink`; `bind` the `sockaddr_un` |
| `connect(fd, sockaddr_vm)` | form path; `connect` the `sockaddr_un` (pass through `EINPROGRESS` for non-blocking fds) |
| `accept(fd)` | `accept` the `AF_UNIX` fd; the accepted fd is a vsock conn too |
| `getsockname`/`getpeername` | synthesize a `sockaddr_vm` from recorded `(cid,port)` |
| `close(fd)` | drop the fd record |

`listen`, `read`/`write`, `send`/`recv` need no interception — they act on the
underlying `AF_UNIX` fd unchanged.

Best-effort caveat: the connector's CID cannot be recovered over `AF_UNIX`, so
`getpeername` on an accepted fd reports the host CID and port `0`. Consumers that
only read/write the accepted fd don't care.

## Adapter tiers

How a consumer engages the convention, by how much of it you can change:

1. **Interposer (runtime)** — inject the interposer dylib via
   `DYLD_INSERT_LIBRARIES`; unmodified `AF_VSOCK` calls are backed. Needs a
   dynamically-linked, non-SIP-restricted binary.
2. **Library (link-time)** — link the same dylib as an ordinary dependency; dyld
   honours its `__interpose` section at launch. No env var, no call-site changes;
   only widen the platform gate and add the library to the link line.
3. **Helpers** — call the `vsock_unix_*` helpers directly (no interception).
4. **Freestanding** — no libc; implement the convention with raw syscalls.

Tiers 1–2 are macOS-specific (they rely on dyld interposition). On Linux, real
`AF_VSOCK` is used and no backing is needed.

## Non-goals

- CID/port realism, `VMADDR_PORT_ANY` auto-assignment, datagram (`SOCK_DGRAM`)
  vsock.
- A security boundary — Unix-socket file permissions are the only access control
  (base dir is `0700`).
- Talking to a real hypervisor/enclave: the emulation is host-local only. On
  Linux, keep real `AF_VSOCK` for that.
