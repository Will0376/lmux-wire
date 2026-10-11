#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd "$(dirname "$0")" && pwd -P) || exit 1

LMUXD=${LMUXD:-"$SCRIPT_DIR/lmuxd"}
CARRIER=${LMUX_CARRIER:-/dev/ttyGS0}

if [ ! -x "$LMUXD" ]; then
    echo "lmuxd not found or not executable: $LMUXD" >&2
    exit 1
fi

while [ ! -c "$CARRIER" ]; do
    sleep 1
done

exec "$LMUXD" \
    --supervisor \
    --outer \
    --carrier "$CARRIER"
