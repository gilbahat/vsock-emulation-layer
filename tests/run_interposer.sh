#!/bin/sh
# Interposer self-test: the *unchanged* AF_VSOCK program tests/vsock_echo talks
# to itself on macOS purely through the injected interposer. The server binds
# VMADDR_CID_ANY (canonicalised to the host CID) and the client dials
# VMADDR_CID_HOST -- they must rendezvous on the same convention path.
set -eu

here=$(cd "$(dirname "$0")/.." && pwd)
dylib="$here/libvsock_unix.dylib"
echo_bin="$here/tests/vsock_echo"
work=$(mktemp -d "${TMPDIR:-/tmp}/vsock-interposer.XXXXXX")
export VSOCK_UNIX_DIR="$work/sock"
trap 'rm -rf "$work"; kill $SRV 2>/dev/null || true' EXIT

PORT=1234
MSG="hello from unchanged AF_VSOCK code"

DYLD_INSERT_LIBRARIES="$dylib" "$echo_bin" server $PORT >/dev/null 2>"$work/srv_err" &
SRV=$!

# Server binds VMADDR_CID_ANY -> canonical host CID (2).
sockpath="$VSOCK_UNIX_DIR/2.$PORT"
i=0
while [ ! -S "$sockpath" ] && [ $i -lt 200 ]; do
	sleep 0.01
	i=$((i + 1))
done
if [ ! -S "$sockpath" ]; then
	echo "FAIL: interposed server never bound $sockpath" >&2
	cat "$work/srv_err" >&2 || true
	exit 1
fi

out=$(DYLD_INSERT_LIBRARIES="$dylib" "$echo_bin" client $PORT "$MSG" 2>"$work/cli_err")
rc=$?

if [ $rc -eq 0 ] && [ "$out" = "$MSG" ]; then
	echo "PASS: unchanged AF_VSOCK echo round-tripped through the interposer"
	exit 0
fi
echo "FAIL: rc=$rc out='$out' expected='$MSG'" >&2
cat "$work/cli_err" >&2 || true
exit 1
