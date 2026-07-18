# vsock-emulation-layer

Emulate Linux `AF_VSOCK` over Unix domain sockets on **macOS**, where there is
no usable vsock (macOS 26 ships the `<sys/vsock.h>` ABI types but
`socket(AF_VSOCK, …)` still fails with *"Operation not supported by device"*).

It exists to unblock local (Mac) development and testing of software that talks
vsock — VM/enclave agents and brokers, unikernels, and anything else using
`AF_VSOCK` — without changing that software.

## The one idea: a `(cid, port)` → path convention

Every vsock endpoint maps to a Unix socket path:

```
${VSOCK_UNIX_DIR:-${TMPDIR:-/tmp}/vsock-unix}/<cid>.<port>
```

A listener binds that path; a connector dials it. Because **all** the adapters
below agree on this one rule, every consumer interoperates — a freestanding
guest can connect to a path a broker is listening on, `vsock-emu` can be either
end, and so on.

- CID realism is intentionally dropped: `(cid,port)` is just a path key.
- `VMADDR_CID_ANY` (`0xFFFFFFFF`) is canonicalised to `VMADDR_CID_HOST` (`2`)
  for path formation, so a server bound to `CID_ANY` and a client dialling the
  host CID meet on the same path (the common host-service topology).
- Well under macOS's 104-byte `sun_path` limit (~37 chars in practice).

## What's here

| File | Role |
| --- | --- |
| `vsock_unix.h` | Header-only. macOS-safe `AF_VSOCK`/`sockaddr_vm` shim (prefers the SDK's `<sys/vsock.h>` when present) **plus** the convention helpers: `vsock_unix_socket`, `vsock_unix_bind`, `vsock_unix_bind_listen`, `vsock_unix_connect`, `vsock_unix_path`. |
| `interpose.c` → `libvsock_unix.dylib` | **Interposer**, usable two ways: injected at runtime (tier 1) *or* linked as a normal dependency (tier 2, "library mode"). Interposes `socket`/`bind`/`connect`/`accept`/`getsockname`/`getpeername`/`close`; backs `AF_VSOCK` with `AF_UNIX`. Built with an `@rpath` install name so the linked form resolves. |
| `vsock-emu.c` → `vsock-emu` | Host peer CLI: `listen`/`connect` on the convention and pump stdin↔socket, so a human or test can be the other end. |
| `tests/vsock_echo.c` | A plain, **unchanged** Linux-style `AF_VSOCK` echo server+client — the representative consumer used to verify the interposer. |

## Four adapter tiers

Pick by how much of the consumer you can change. **Which tier?** Can't touch the
binary → interposer. Can change the build but not the source → library mode. Can
change the source → helpers. Freestanding/no-libc → convention by hand.

Most software gates its vsock code behind `#ifdef __linux__`, so on macOS it is
compiled out entirely. Tiers 2–3 assume you first **widen that gate** to include
macOS — see [Porting a gated consumer](#porting-a-gated-consumer).

### 1. Interposer — runtime, zero build changes

Inject the dylib at launch; unmodified `AF_VSOCK` calls are backed by AF_UNIX.

```sh
DYLD_INSERT_LIBRARIES=$PWD/libvsock_unix.dylib ./your-vsock-program
```

Works for dynamically-linked, non-SIP-restricted programs. The env var can be
stripped (e.g. by a launcher) and SIP-restricted binaries ignore it — if that
bites, use tier 2.

### 2. Library mode — link-time, no source (call-site) changes

For callers willing to change their **build system** but not their code: link
the same dylib as an ordinary dependency. dyld honours a linked dylib's
`__interpose` section at launch, so there is **no `DYLD_INSERT_LIBRARIES`** and
**no call-site changes** — you only widen the platform gate and add the library
to your link line.

```sh
cc -c app.c -o app.o           # gate now includes __APPLE__ (types via vsock_unix.h)
cc app.o -o app -L/path/to/lib -Wl,-needed-lvsock_unix -Wl,-rpath,/path/to/lib
```

Caveats:
- `-Wl,-needed-lvsock_unix` (or `-Wl,-needed_library,<abspath>`) is recommended:
  the app references **no** symbol from the dylib, so a linker configured to
  dead-strip unused dylibs (`-dead_strip_dylibs`) would drop it and interposition
  would never load. Plain `-lvsock_unix` also works on current Apple toolchains
  (unused dylibs are kept by default); `-needed` just makes it robust.
- Provide an rpath (or install the dylib to a standard path) so it resolves at
  runtime.
- macOS/dylib-only: a static archive is not honoured for interposition (dyld does
  not interpose the main image's own calls), and Linux uses real vsock so no
  library is needed there.
- The vsock code must actually be **compiled in** on macOS (hence the gate
  change) — interposition has nothing to catch if the calls were `#ifdef`'d out.

### 3. Recompile to the helpers — no interposition at all

Include `vsock_unix.h` and call the `vsock_unix_*` helpers
(`vsock_unix_socket`/`_bind`/`_bind_listen`/`_connect`) directly. Nothing to
inject or link; the calls create AF_UNIX sockets over the convention outright.

### 4. Freestanding raw-syscall code

No libc to interpose or link — implement the convention with raw syscalls. See
[docs/DESIGN.md](docs/DESIGN.md) for the exact rules (a unikernel guest binding
that issues `socket`/`connect`/`accept` directly is a typical case).

## Porting a gated consumer

Widen the platform gate to include macOS and take the `AF_VSOCK`/`sockaddr_vm`
types from `vsock_unix.h` (which prefers the SDK's `<sys/vsock.h>` and falls back
to a shim). **No other code changes** — the raw socket calls stay as-is, backed
by tier 1 or 2 on macOS and by real vsock on Linux:

```c
/* before */                       /* after */
#if defined(__linux__)             #if defined(__linux__) || defined(__APPLE__)
# include <linux/vm_sockets.h>     # if defined(__APPLE__)
                                   #  include "vsock_unix.h"      /* types */
                                   # else
                                   #  include <linux/vm_sockets.h>
                                   # endif
  int fd = socket(AF_VSOCK, …);      int fd = socket(AF_VSOCK, …);   /* unchanged */
  bind(fd, &svm, …);                 bind(fd, &svm, …);             /* unchanged */
#endif                             #endif
```

## Build & test

```sh
make            # builds vsock-emu, tests/vsock_echo, libvsock_unix.dylib
make test       # runs the self-tests
```

- `tests/run_convention.sh`: two `vsock-emu` peers exchange data over the
  convention — exercises the helpers directly, no interposer, no AF_VSOCK.
- `tests/run_interposer.sh`: the unchanged `AF_VSOCK` `vsock_echo` talks to
  itself purely through the interposer.

### `vsock-emu` by hand

```sh
# terminal A — be a listener on (cid=2, port=1234), echo-ish via cat
./vsock-emu listen 2 1234

# terminal B — dial it
./vsock-emu connect 2 1234
```

Set `VSOCK_UNIX_DIR` to control where the socket files live (defaults to
`${TMPDIR:-/tmp}/vsock-unix`).

## Verification

The convention is exercised end-to-end by `make test` and has been validated
against real `AF_VSOCK` consumers spanning all four tiers — libc programs (both
interposed and linked) and a freestanding, raw-syscall unikernel guest —
interoperating over a single shared path. The freestanding case in particular
closes the "can't test vsock on a Mac" gap that motivated the project. See
[docs/DESIGN.md](docs/DESIGN.md) for the interception surface a new adapter must
implement.

## Limitations

- Interposition (tiers 1 and 2) affects dynamically-linked programs only;
  raw-syscall code is not interposed (by design — that's what tier 4 is for).
  Tier 1 additionally needs a non-SIP-restricted binary and an intact
  `DYLD_INSERT_LIBRARIES`; tier 2 sidesteps both by linking at build time.
- `getpeername` on an accepted fd is best-effort: the connector's CID can't be
  recovered over `AF_UNIX`, so it reports the host CID and port 0. Consumers
  that only read/write the accepted fd (most) don't care.
- `dup`/`dup2` of a vsock fd are not tracked; use the original fd.
- Interception (tiers 1–2) is macOS-specific; the `vsock_unix_*` helpers are
  plain POSIX. On Linux, use real `AF_VSOCK` — no emulation needed.

## License

ISC — see [LICENSE](LICENSE). Contributions welcome; see
[CONTRIBUTING.md](CONTRIBUTING.md).
