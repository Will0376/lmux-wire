#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd "$(dirname "$0")" && pwd -P) || exit 1

LMUXD=${LMUXD:-"$SCRIPT_DIR/lmuxd"}
CARRIER=${LMUX_CARRIER:-/dev/ttyGS0}
CARRIER_NAME=${LMUX_CARRIER_NAME:-primary}
STANDBY_CARRIER=${LMUX_STANDBY_CARRIER:-}
STANDBY_BAUD=${LMUX_STANDBY_BAUD:-115200}
STANDBY_RTSCTS=${LMUX_STANDBY_RTSCTS:-0}
STANDBY_DISPATCH=${LMUX_STANDBY_DISPATCH:-}

if [ ! -x "$LMUXD" ]; then
    echo "lmuxd not found or not executable: $LMUXD" >&2
    exit 1
fi

set -- --supervisor --outer --carrier "$CARRIER" --carrier-name "$CARRIER_NAME"

# Device availability is handled independently by the supervisor reopen loop.
# A missing primary device must not prevent a named standby from opening.
if [ -n "$STANDBY_CARRIER" ] && [ -n "$STANDBY_DISPATCH" ]; then
    echo "set only one of LMUX_STANDBY_CARRIER or LMUX_STANDBY_DISPATCH" >&2
    exit 1
elif [ -n "$STANDBY_DISPATCH" ]; then
    set -- "$@" --standby-dispatch "$STANDBY_DISPATCH"
elif [ -n "$STANDBY_CARRIER" ]; then
    set -- "$@" --standby-carrier "$STANDBY_CARRIER" \
        --standby-baud "$STANDBY_BAUD"
    if [ "$STANDBY_RTSCTS" = 1 ]; then
        set -- "$@" --standby-rtscts
    fi
fi

exec "$LMUXD" "$@"
