# UART console dispatcher

`lmux-uart-dispatch` is a small, platform-independent sole-reader owner for a
console-capable serial TTY. It is used only for a DIRTY-capable named carrier.
It does not implement LMC1 transactions, OUTER, LMUX, a terminal gateway, or
platform service-manager policy.

## Process and TTY contract

The dispatcher accepts exactly one physical TTY, either with `--tty PATH` or
with `--tty-fd FD`. The inherited-fd form is intended for an init/service
manager which has already assigned the terminal. `--acquire-ctty` requests a
new session/controlling terminal without stealing an existing controlling
terminal. The platform integration must remove or stop every competing
userspace RX consumer before starting the dispatcher. `TIOCEXCL` is used as a
guard against later opens, not as proof that no earlier reader exists.

The startup interface is:

```text
lmux-uart-dispatch (--tty PATH | --tty-fd FD) --ipc-listen PATH
    [--peer-uid UID] [--acquire-ctty]
    [--active-loss console|idle|exit]
    [--trace-file REGULAR_FILE]
    [--console-exec /absolute/program [ARG...]]
```

`--console-exec` and all following words are passed directly to `execve`.
There is no shell interpretation. The executable must be absolute. The
default authenticated supervisor UID is the dispatcher's effective UID and
may be set explicitly with `--peer-uid`. The socket is created mode 0600. The
supervisor connector independently requires mode 0600 and verifies that
`SO_PEERCRED.uid` equals the socket owner's UID.

`--trace-file` is an opt-in physical-boundary diagnostic. The path must resolve
to a regular file and is opened mode 0600 with `O_NOFOLLOW`; it cannot be the
UART. The file is truncated at startup and accepts at most 256 fixed-size
metadata lines (at most 81,664 bytes). Each target-to-UART record records a
monotonic timestamp, local record ID, C1/D1 classification, encoded length and
FNV diagnostic hash. A valid D1 additionally records type, session, packet and
fragment identifiers. Separate lines record complete LDP1 receipt, termios
`c_oflag`, every accepted TTY write range, completion, errors, IPC connect/loss,
and ACTIVE/IDLE/RELEASE. Payload bytes are not dumped. Trace write failure
disables tracing and does not change carrier semantics.

The ARM EABI5 static dispatcher is 519,072 bytes in the B4.2 build. Its main
frame is 2640 bytes; the optional trace formatter's maximum frame is 364 bytes.
Tracing adds no record history in RAM and the D1 diagnostic decoder is invoked
only when a trace file is enabled.

At startup the dispatcher snapshots the complete termios state and file flags.
It makes RX noncanonical, non-echoing, and binary-clean while preserving the
configured baud, character format, flow control, and output processing. It
restores the snapshot before console handoff or process exit. Exit status 0 is
a requested clean exit, 70 is startup/configuration failure, and 71 is a
runtime/IPC/TTY failure. Successful console handoff replaces the process, so
the eventual status is the configured console program's status.

The dispatcher has three logical states:

- IDLE classifies bounded C1/D1 records and waits for a supervisor; ordinary
  console text is discarded except for the explicit console command.
- LMUX_ACTIVE forwards C1/D1 records and treats unexpected supervisor IPC
  loss as a console request when a console executable is configured.
- CONSOLE_ACTIVE is an `execve`, not a proxy state. The dispatcher no longer
  exists and no longer owns RX.

Consequently automatic LMC1 takeover while an independent login/shell owns the
TTY is not supported. Supporting that would require a permanent PTY gateway,
which is deliberately outside this milestone.

## Local IPC

The target supervisor connects with `--standby-dispatch NAME=SOCKET`. The
transport is Unix `SOCK_SEQPACKET`; there is one authenticated peer and one
bounded message in each direction. Every packet has this little-endian header:

```text
u32 magic       "LDP1"
u8  version     1
u8  type
u16 payload_len
u8  payload[payload_len]
```

Payload is bounded by `LMUX_DIRTY_RECORD_MAX` (327 bytes). Types are
`RX_RECORD`, `TX_RECORD`, `ACTIVE`, `IDLE`, and `RELEASE`. The last three have
no payload. `MSG_TRUNC`, malformed headers, unexpected types, partial packet
sends, and active EOF are explicit failures. A reconnect resets parser,
records, queues, and control state; stale messages are not retained.

The dispatcher only classifies complete `~LMUX-C1:` and `~LMUX-D1:` physical
records. Existing LMC1 and DIRTY decoders in the supervisor validate their
contents and CRCs. Thus LMC1 transaction state remains supervisor-owned.
`ACTIVE` is sent only after COMMITTED has been handed to the dispatcher;
`IDLE` is sent when authority moves elsewhere. Normal IDLE never starts a
console.

## Physical output and emergency escape

C1/D1 logical records end in CRLF. If output processing is disabled, the
dispatcher writes those bytes unchanged. With `OPOST|ONLCR`, it removes the
logical CR and writes the final LF; the kernel line discipline then emits
exactly one physical CRLF. Other enabled output flags which can change bytes
or insert delays are rejected. The dispatcher never toggles `OPOST` around a
write.

The exact physical line `~LMUX CONSOLE`, terminated by CR, LF, or CRLF,
requests console handoff. CRLF is consumed as one ending. Recognition is
disabled while a C1/D1 record is being collected, so payload/base64 text
cannot trigger it. RAW carriers do not use this dispatcher.

TTY write serialization does not prove physical-record atomicity against the
kernel serial console. Userspace writes pass through the TTY line discipline
and UART transmit queue, while `printk` console output uses the serial console
driver path. Whether the particular low-level driver can insert console bytes
between bytes accepted by one userspace `write()` is platform/driver specific.
The optional trace therefore distinguishes “dispatcher did not finish the
write” from “dispatcher finished the exact logical record but the host observed
a different record”; it does not claim to observe bytes after the UART hardware
queue.

On the host, the sole carrier owner exposes:

```sh
lmux carrier console NAME [--control SOCKET]
```

It does not require an established OUTER or LMUX connection. It retires the
local incarnation, discards unsent protocol records, then writes an explicit
abort boundary followed by `~LMUX CONSOLE\r\n`. A successful reply means the
complete escape sequence was accepted by the physical carrier write path and
the host endpoint was released; it is not an acknowledgment from the target
shell. The endpoint must be the active named DIRTY carrier. The host keeps the
released endpoint closed, so its normal reopen loop cannot steal RX back from
login. A later explicit `lmux carrier switch NAME --mode dirty` clears that
hold and is the resume operation after logout/dispatcher respawn.

## Platform integration examples

These are service-manager examples, not dispatcher defaults. In every case
the existing serial getty unit/entry must not run concurrently.

BusyBox init can provide the terminal on fd 0:

```text
ttyS0::respawn:/usr/sbin/lmux-uart-dispatch --tty-fd 0 --acquire-ctty --ipc-listen /run/lmux/uart.sock --console-exec /sbin/getty -L ttyS0 115200 vt100
```

For OpenWrt procd, define one respawned foreground instance, give it the TTY
as inherited stdio (or use `--tty`), and disable the corresponding console
getty in the platform configuration. Set the same supervisor socket with
`--standby-dispatch`. Procd owns respawn/fallback; the binary contains no
procd-specific branch.

For systemd, a foreground service may use `StandardInput=tty`,
`StandardOutput=tty`, `TTYPath=/dev/ttyS0`, and `--tty-fd 0`; mask the matching
`serial-getty@ttyS0.service`. The unit controls restart policy. Do not combine
this with a simultaneously active serial-getty unit.

For a manual foreground laboratory check from an already authenticated serial
shell, use `exec` so the shell itself is replaced and no second RX reader is
left behind:

```sh
exec /root/lmux/lmux-uart-dispatch \
  --tty-fd 0 --ipc-listen /tmp/lmux-uart.sock \
  --console-exec /bin/sh
```

When the replacement shell exits, the platform's init respawn chain regains
its usual getty/login lifecycle. This direct shell example is only for an already
authenticated, trusted physical laboratory session.

## Hantek DSO2D15 integration baseline

Hantek is an integration example, not a production-code identity. Its BusyBox
1.26.2 configuration uses `console::respawn`, `/dev/console` resolves to the
kernel console on ttyS0, and the current command is
`/sbin/getty -L console 115200 vt100`. Kernel/phoenix TX remains visible and
is tolerated by DIRTY. A future persistent entry would replace that one getty
entry with the generic dispatcher and use the same getty command as
`--console-exec`; no Hantek path or command is compiled into the dispatcher.

The first hardware acceptance must not edit inittab. From an authenticated
console, first copy and checksum the binary, verify `--help`, save a copy of
the current configuration, then use the manual foreground `exec` form above.
Verify IDLE escape, DIRTY activation, host `carrier console`, termios
restoration, logout, and return to the stock getty. Only after that reversible
test should a separately authorized run stage an init change. Rollback remains
the untouched original inittab plus BusyBox respawn/reboot policy.

For the B4.2 physical-TX diagnostic run, the reversible foreground command adds
a regular-file trace before `--console-exec`, for example:

```sh
exec /root/lmux/lmux-uart-dispatch \
  --tty-fd 0 \
  --ipc-listen /tmp/lmux-uart.sock \
  --trace-file /tmp/lmux-uart-b42.trace \
  --console-exec /sbin/getty -L console 115200 vt100
```

Before removing USB, verify that the running dispatcher PID resolves to the
new executable (not an old unlinked inode), the trace contains `tty_ready` and
`ipc_connect`, the supervisor's dispatcher endpoint is open/standby, and the
host primary OUTER/LMUX is established. After exactly one authorized DIRTY
switch, preserve both the host `carrier_dirty_rxdiag` status and this trace.
Matching D1 session/sequence/fragment metadata plus a completed target hash but
a different host invalid hash localizes the change after the dispatcher record
boundary; a missing `tx_complete`, write error, or IPC disconnect localizes it
earlier. Do not emit or inspect this trace through the UART itself.

Zeromux is unchanged. A future bootstrap signature can be added as another
bounded classifier owned by this sole RX process; it must not depend on an
established OUTER/LMUX connection.
