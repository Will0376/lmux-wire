#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd "$(dirname "$0")" && pwd -P) || exit 1

LMUXD=${LMUXD:-"$SCRIPT_DIR/lmuxd"}
CARRIER=${LMUX_CARRIER:-/dev/ttyGS0}
GDB_SOCKET=${LMUX_GDB_SOCKET:-/tmp/lmux-gdb.sock}

if [ ! -x "$LMUXD" ]; then
echo "lmuxd not found or not executable: $LMUXD" >&2
exit 1
fi

while [ ! -e "$CARRIER" ]; do
sleep 1
done

exec "$LMUXD"
--carrier "$CARRIER"
--shell 1:/dev/lmux1
--shell 2:/dev/lmux2
--socket 3:"$GDB_SOCKET"
--file 4
