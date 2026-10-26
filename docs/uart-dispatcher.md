# UART console dispatcher

`lmux-uart-dispatch` is a small, platform-independent sole-reader owner for a
console-capable serial TTY. It is used only for a DIRTY-capable named carrier.
It does not implement LMC1 transactions, OUTER, LMUX, a terminal gateway, or
platform service-manager policy.

## Shared-console DIRTY deployment

**A shared-console DIRTY carrier is a deliberately prepared recovery facility,
not a drop-in replacement for a RAW UART and not just another serial device
path.** Both ends must be configured and the target ownership/IPC path must be
running before it can recover a failed primary carrier.

RAW and shared-console DIRTY solve different deployment problems:

| Property | RAW UART | Shared-console DIRTY UART |
| --- | --- | --- |
| Physical stream | Dedicated, binary-clean, ordered full duplex | ASCII/Base64 C1/D1 records mixed with possible foreign console TX |
| Serial setup | Strict baud, 8N1, flow control and binary-clean termios | Matching baud, 8N1 and flow control; dispatcher preserves/restores console termios and encodes physical records |
| RX ownership | Exactly one LMUX userspace reader | Exactly one dispatcher userspace reader; supervisor uses local IPC |
| Unsolicited TX | Unsupported; the line must be clean | Tolerated only within integrity, retry and deadline bounds |
| OUTER mapping | Identity adapter for encoded OUTER bytes | Integrity-checked, ordered packets with bounded retransmission |
| Console dispatcher | Not required on a dedicated serial endpoint | Required when LMUX shares the physical console |

DIRTY detects corruption and either delivers exact ordered OUTER bytes or
fails the carrier. It is not transparent serial-console multiplexing and does
not guarantee progress under continuous interference. Kernel and application
console **TX** may coexist within those limits, but independent userspace RX
readers may not. In particular, running getty and LMUX as two readers of the
same TTY is unsupported: input can be consumed by either process.

Before relying on a rescue carrier, prepare all of the following:

1. Configure the same bounded, case-sensitive logical carrier name on host and
   target; local device and socket paths need not match.
2. Configure compatible baud, 8N1 and hardware-flow-control settings.
3. Establish exactly one userspace RX owner for the target physical UART.
4. Run `lmux-uart-dispatch` with ownership of that TTY.
5. Connect the target supervisor to the dispatcher through its configured
   local `SOCK_SEQPACKET` endpoint.
6. Configure host `lmuxd` with the corresponding named rescue TTY endpoint.
7. Verify emergency console handoff before depending on LMUX recovery.
8. Perform a controlled primary-loss recovery test on the actual platform.

An open standby UART endpoint on the host proves only that the host opened its
local device. It does **not** prove that target-side DIRTY recovery is ready.
The dispatcher process, sole-RX ownership, local IPC connection, target
supervisor, and host endpoint must all be checked.

## Process and TTY contract

The dispatcher accepts exactly one physical TTY, either with `--tty PATH` or
with `--tty-fd FD`. The inherited-fd form is intended for an init/service
manager which has already assigned the terminal. `--acquire-ctty` requests a
new session/controlling terminal without stealing an existing controlling
terminal. The platform integration must remove or stop every competing
userspace RX consumer before starting the dispatcher. `TIOCEXCL` is used as a
guard against later opens, not as proof that no earlier reader exists.

For example, `lmux-uart-dispatch --tty-fd 0` uses the caller's file descriptor
0; it does not discover a physical UART. When invoked by the service which owns
the actual serial console, fd 0 may be the intended TTY. When invoked inside
`lmux term`, fd 0 is normally an LMUX PTY, so the command does not arm the
physical UART. Do not open `/dev/ttyS0` (or another platform TTY) from an
arbitrary LMUX shell while getty/login already owns it. Inheriting or opening a
TTY establishes an fd ownership path; acquiring a controlling terminal,
session leadership, and job-control semantics is a separate platform concern.

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
console-released endpoint closed, so its normal reopen loop cannot steal RX
back from login. This console-release hold is separate from the carrier
manager's administrative `RELEASED` state. A later explicit
`lmux carrier switch NAME --mode dirty` clears the console-release hold and is
the console lifecycle's resume operation after logout/dispatcher respawn;
`lmux carrier resume NAME` clears only an administrative hold and does not
perform a switch.

If host `lmuxd` is unavailable and no longer owns the host UART, an operator
may send the physical command directly, for example on one particular
platform:

```sh
printf '%s\r\n' '~LMUX CONSOLE' > /dev/ttyUSB0
```

The device path is platform-specific. Never add a concurrent writer while
host `lmuxd` still owns the endpoint; releasing or stopping that owner is an
explicit operator decision. `TIOCEXCL` is not absolute evidence of sole
ownership: descriptors opened earlier remain valid, and a privileged
`CAP_SYS_ADMIN` process may bypass it. A successful local write proves only
that the bytes were accepted locally, not that the target recognized them or
successfully executed getty.

On a successful escape the dispatcher closes IPC, abandons the active DIRTY
incarnation, restores the saved console termios, and calls `execve()` for the
configured getty/login. The dispatcher no longer exists after that successful
handoff. Returning to DIRTY requires the platform service manager to start a
new dispatcher instance, normally after logout.

## Explicit carrier recovery

LMUX performs no automatic carrier fallback. A human operator or an explicitly
authorized agent initiates recovery, for example:

```sh
lmux carrier switch rescue --mode dirty
```

Success means an authoritative target COMMIT, fresh OUTER establishment, a
fresh LMUX incarnation, and desired-service reconciliation. A failed
transaction can leave `committed-but-down` or `protocol_failed`; automatic
rollback to the old carrier is not guaranteed. A target worker and an existing
structured EXEC may survive a carrier failure, but process survival and
successful transport/LMUX reconnection are separate facts.

## Platform service-manager integration

The platform integration, not the dispatcher binary, chooses the serial
endpoint, removes the competing getty, provides controlling-terminal/session
semantics, configures the console command, and defines restart/fallback policy.
The dispatcher must become the configured serial-console RX owner. The target
supervisor is a separate process and must not inherit physical-console stdin.

The following is an explicitly platform-specific **BusyBox init example**:

```ini
console::respawn:/root/startlmux-uart-rescue.sh
::respawn:/root/lmux/start-supervisor.sh
```

Example dispatcher launcher:

```sh
#!/bin/sh

exec /root/lmux/lmux-uart-dispatch \
  --tty-fd 0 \
  --ipc-listen /tmp/lmux-uart.sock \
  --trace-file /tmp/lmux-uart.trace \
  --console-exec /sbin/getty -L console 115200 vt100
```

Example supervisor launcher:

```sh
#!/bin/sh

exec /root/lmux/lmuxd \
  --supervisor --outer \
  --carrier /dev/ttyGS0 \
  --standby-dispatch rescue=/tmp/lmux-uart.sock \
  --verbose \
  </dev/null >>/tmp/lmux-supervisor.log 2>&1
```

All paths, logical names, TTY assumptions, logging destinations, baud rates,
and getty arguments above are examples rather than portable defaults. Use
`exec`, not backgrounding, so the service manager supervises the actual
process. There must be only one dispatcher/getty reader and no duplicate
console respawn entry. Successful console handoff replaces the dispatcher with
getty. After logout, the service manager must restart the dispatcher; while
getty/login owns RX, new LMC1 takeover is impossible without a permanent
gateway (which LMUX does not provide).

`respawn` restarts a process which exits; it does not repair a process which is
hung. Replacing an existing console startup entry therefore carries a lockout
risk: a missing binary, broken launcher, wrong fd or controlling-TTY
assumption, or stalled dispatcher can remove the only interactive recovery
channel. BusyBox respawn is not a complete fail-safe. Never edit
`/etc/inittab` automatically; first validate the reversible foreground path
and prepare an independently usable rollback.

For OpenWrt procd, define one respawned foreground instance, provide the TTY as
inherited stdio (or use `--tty`), disable the corresponding console getty, and
configure the supervisor with the same `--standby-dispatch` socket. For
systemd, a service may use `StandardInput=tty`, `StandardOutput=tty`, and
`TTYPath=...` with `--tty-fd 0`; mask the matching `serial-getty@...` unit. In
both cases the service manager, not the generic dispatcher, owns restart and
fallback policy.

For a manual foreground laboratory check from an already authenticated
physical serial shell, use `exec` so the shell itself is replaced and no
second RX reader remains:

```sh
exec /root/lmux/lmux-uart-dispatch \
  --tty-fd 0 \
  --ipc-listen /tmp/lmux-uart.sock \
  --console-exec /bin/sh
```

When the replacement shell exits, the platform's init respawn chain can regain
its usual getty/login lifecycle. Direct shell handoff is appropriate only for
an already authenticated, trusted physical laboratory configuration.

## Failure boundaries

| Condition | LMUX guarantee | Operator or platform responsibility |
| --- | --- | --- |
| Primary USB is lost while supervisor and dispatcher are healthy | Existing OUTER/LMUX retires; no automatic fallback | Explicitly switch to the prepared DIRTY endpoint and verify full reconciliation |
| DIRTY retries/deadline are exhausted | No unchecked bytes reach OUTER; the carrier fails explicitly | Inspect interference, ownership and endpoint health; start a new explicit transaction if appropriate |
| Target supervisor fails or is replaced | Dispatcher cannot recreate supervisor/LMC1 state | Service manager restores supervisor; carrier and worker outcomes follow their separate lifecycles |
| Dispatcher exits | Active DIRTY IPC closes and fails; an idle standby disappears | Service manager may restart it; exit alone does not establish recovery readiness |
| Dispatcher hangs | No watchdog or self-repair guarantee | Platform watchdog/operator must recover it without introducing a second RX reader |
| Another userspace process reads the UART | Behavior is unsupported; DIRTY cannot reconstruct stolen RX bytes | Remove the competing reader and re-establish sole ownership |
| Host UART remains owned by `lmuxd` | Host CLI can serialize `carrier console`; another writer is unsafe | Do not write the device directly until ownership is explicitly released |
| Dispatcher hands the console to getty | Termios is restored and dispatcher is replaced by getty | After logout, service manager restarts dispatcher; no LMC1 takeover exists meanwhile |
| Kernel/application console TX corrupts records | Corruption is detected; bounded retransmission may recover it | Continuous/excessive interference can still fail the carrier; platform behavior is driver-dependent |

DIRTY cannot restore a missing/broken supervisor executable, a permanently
stalled dispatcher, or an interrupted boot process. Zeromux is a separate
bootstrap/recovery mechanism and is not part of DIRTY framing.

## Operational readiness checklist

Copy this list into the platform acceptance record before relying on DIRTY
recovery:

- [ ] Host and target use the same logical rescue name and compatible serial
      settings; their local path/socket mappings are recorded.
- [ ] Exactly one target userspace process owns physical UART RX; competing
      getty/login/shell readers are absent.
- [ ] The dispatcher is running on the intended physical TTY, not an LMUX PTY.
- [ ] The supervisor is running independently, its dispatcher IPC endpoint is
      connected, and the worker is healthy.
- [ ] Host `lmuxd` has the named rescue endpoint open/standby under sole host
      ownership.
- [ ] `~LMUX CONSOLE` and `lmux carrier console NAME` restore the intended
      console, and the service manager subsequently restores the dispatcher.
      **Controlled physical test required.**
- [ ] Primary/USB to rescue/DIRTY reaches fresh OUTER, fresh LMUX, and service
      reconciliation. **Controlled physical test required.**
- [ ] Rescue/DIRTY to primary/RAW recovery is verified. **Controlled physical
      test required.**
- [ ] Expected worker and structured-EXEC survival/loss behavior has been
      tested and documented separately from carrier recovery. **Controlled
      failure test may be required.**

## Hantek DSO2D15 integration baseline

Hantek is an integration example, not a production-code identity. Its BusyBox
1.26.2 configuration uses `console::respawn`, `/dev/console` resolves to the
kernel console on ttyS0, and the current command is
`/sbin/getty -L console 115200 vt100`. Kernel/phoenix TX remains visible;
DIRTY may recover bounded interference but does not guarantee progress under
continuous output. A future persistent entry would replace that one getty
entry with the generic dispatcher and use the same getty command as
`--console-exec`; no Hantek path or command is compiled into the dispatcher.

The first hardware acceptance must not edit inittab. From an authenticated
console, first copy and checksum the binary, verify `--help`, save a copy of
the current configuration, then use a manual foreground `exec` with the
platform getty as `--console-exec` (as in the example below). Verify IDLE
escape, DIRTY activation, host `carrier console`, termios restoration, logout,
and return to the stock getty. Only after that reversible test should a
separately authorized run stage an init change. Keep the original inittab
untouched and prepare an independently usable rollback path; BusyBox respawn
alone is not a complete lockout safeguard.

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
