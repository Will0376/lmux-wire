#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd "$(dirname "$0")" && pwd -P) || exit 1

LMUXD=${LMUXD:-"$SCRIPT_DIR/build/host/lmuxd"}
CARRIER=${LMUX_CARRIER:-/dev/ttyACM0}
CARRIER_NAME=${LMUX_CARRIER_NAME:-primary}
STANDBY_CARRIER=${LMUX_STANDBY_CARRIER:-rescue=/dev/ttyUSB0}
STANDBY_BAUD=${LMUX_STANDBY_BAUD:-115200}
STANDBY_RTSCTS=${LMUX_STANDBY_RTSCTS:-0}
RUNDIR=${LMUX_RUNDIR:-/run/lmux}

PIDFILE=$RUNDIR/lmuxd.pid
LOG=$RUNDIR/lmuxd.log

# Re-exec ourselves as root.

if [ "$(id -u)" -ne 0 ]; then
exec sudo "$0" "$@"
fi

if [ ! -x "$LMUXD" ]; then
echo "lmuxd not found or not executable: $LMUXD" >&2
echo "Build it first or set LMUXD=/path/to/lmuxd" >&2
exit 1
fi

if [ ! -e "$CARRIER" ]; then
echo "primary carrier not present yet; lmuxd will retry: $CARRIER" >&2
fi

mkdir -p "$RUNDIR"

# Stop previous instance started by this script.

if [ -f "$PIDFILE" ]; then
OLDPID=$(cat "$PIDFILE" 2>/dev/null || true)
if [ -n "$OLDPID" ]; then
kill "$OLDPID" 2>/dev/null || true
sleep 1
fi
rm -f "$PIDFILE"
fi

# Also clean up a manually started lmuxd if present.

killall lmuxd 2>/dev/null || true
sleep 1

rm -f \
"$RUNDIR/console1.sock" \
"$RUNDIR/console2.sock" \
"$RUNDIR/gdb.sock" \
"$RUNDIR/file.sock" \
"$RUNDIR/control.sock" \
"$RUNDIR/control-v2.sock"

# Make host-side sockets connectable by the normal user.

umask 000

set -- --role host --outer --carrier "$CARRIER" --carrier-name "$CARRIER_NAME"
if [ -n "$STANDBY_CARRIER" ]; then
set -- "$@" --standby-carrier "$STANDBY_CARRIER" \
--standby-baud "$STANDBY_BAUD"
if [ "$STANDBY_RTSCTS" = 1 ]; then
set -- "$@" --standby-rtscts
fi
fi
set -- "$@" \
--control "$RUNDIR/control.sock" \
--structured-control "$RUNDIR/control-v2.sock" \
--pty console1:"$RUNDIR/console1.sock":/bin/sh \
--pty console2:"$RUNDIR/console2.sock":/bin/sh \
--unix gdb:"$RUNDIR/gdb.sock":/tmp/lmux-gdb.sock \
--file files:"$RUNDIR/file.sock" \
--verbose

nohup "$LMUXD" "$@" >"$LOG" 2>&1 &

PID=$!
echo "$PID" >"$PIDFILE"

sleep 1

if ! kill -0 "$PID" 2>/dev/null; then
echo "lmuxd failed to start" >&2
cat "$LOG" >&2
exit 1
fi

echo "host lmuxd pid=$PID"
echo "binary: $LMUXD"
echo "primary carrier: $CARRIER_NAME=$CARRIER"
echo "standby carrier: ${STANDBY_CARRIER:--}"
echo "log: $LOG"
echo
cat "$LOG"
