# Contributing

Thanks for your interest. This is a small, focused facility — the goal is to keep
it tiny, dependency-free, and easy to audit.

## Ground rules

- **No new runtime dependencies.** The core is a header plus a couple of small C
  files; keep it that way.
- **Match the surrounding style.** C, comment density and naming as in the
  existing files. New files start with an SPDX header:
  `/* SPDX-License-Identifier: ISC */`.
- **Preserve the convention.** The `(cid,port)->path` rule in
  [docs/DESIGN.md](docs/DESIGN.md) is the interop contract; changing it breaks
  every consumer. Discuss in an issue before proposing a change to it.
- **By contributing you agree your work is licensed under the project's
  [ISC license](LICENSE).**

## Building and testing

```sh
make          # vsock-emu, tests/vsock_echo, libvsock_unix.dylib
make test     # runs the self-tests
```

Add or update a test under `tests/` for any behavior change and make sure
`make test` passes before opening a pull request. Note the interception tiers
(interposer / library mode) rely on macOS dyld interposition, so those paths are
macOS-only; the `vsock_unix_*` helpers are plain POSIX.

## Reporting issues

Include your macOS version (`sw_vers`), architecture (`uname -m`), and the exact
commands plus output. For interposition problems, `otool -L` of the affected
binary and whether you used the interposer (env var) or library (linked) tier are
especially helpful.
