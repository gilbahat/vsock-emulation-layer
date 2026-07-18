#!/bin/sh
# Convention self-test: two vsock-emu peers exchange data over the (cid,port)
# convention, with no interposer and no AF_VSOCK -- exercising vsock_unix_*
# directly. The listener sends one line, the connector sends another; each
# must receive the other's line.
set -eu

here=$(cd "$(dirname "$0")/.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/vsock-convention.XXXXXX")
export VSOCK_UNIX_DIR="$work/sock"
trap 'rm -rf "$work"; kill $SRV 2>/dev/null || true' EXIT

CID=2
PORT=9000

# The listener sends "from-server"; whatever it *receives* (the client's line)
# lands on its stdout -> server_recv. Symmetrically for the connector.
printf 'from-server\n' | "$here/vsock-emu" listen $CID $PORT >"$work/server_recv" 2>/dev/null &
SRV=$!

# Wait (up to ~2s) for the listener to bind its path.
i=0
while [ ! -S "$VSOCK_UNIX_DIR/$CID.$PORT" ] && [ $i -lt 200 ]; do
	sleep 0.01
	i=$((i + 1))
done
if [ ! -S "$VSOCK_UNIX_DIR/$CID.$PORT" ]; then
	echo "FAIL: listener never bound $VSOCK_UNIX_DIR/$CID.$PORT" >&2
	exit 1
fi

printf 'from-client\n' | "$here/vsock-emu" connect $CID $PORT >"$work/client_recv" 2>/dev/null
wait $SRV 2>/dev/null || true

ok=1
if ! grep -q 'from-server' "$work/client_recv"; then
	echo "FAIL: connector did not receive server's line" >&2
	ok=0
fi
if ! grep -q 'from-client' "$work/server_recv"; then
	echo "FAIL: listener did not receive client's line" >&2
	ok=0
fi

if [ $ok -eq 1 ]; then
	echo "PASS: bidirectional data over (cid=$CID, port=$PORT)"
	exit 0
fi
exit 1
