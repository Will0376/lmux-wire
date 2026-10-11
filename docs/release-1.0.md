# LMUX Wire 1.0 Release Contract

This document freezes the LMUX Wire 1.0 public contract. It describes the
supported surface, the internal surfaces that may change without a SemVer major,
known limitations, and the release verification gate.

## Baseline

LMUX Wire 1.0 is a userspace transport and service stack for embedded Linux.
The hardware baseline is Hantek DSO2D15 class hardware:

```text
CPU:      ARM926EJ-S / ARMv5TEJ
kernel:   Linux 5.2
libc:     glibc 2.23
userland: BusyBox
memory:   about 55 MiB RAM, no swap
ABI:      EABI5 soft-float
build:    static target binary
```

Default paths:

```text
host carrier:    /dev/ttyACM0
target carrier:  /dev/ttyGS0
target install:  /root/lmux/
host runtime:    /run/lmux/
```

LMUX Wire 1.0 does not require or provide `n_gsm`, USB networking, kernel
modules, or a threaded target daemon. The target remains C and the daemon uses
one process and one `poll()` loop.

License: Mozilla Public License 2.0 (MPL-2.0).

## SemVer

`1.0.0` defines the public API. After 1.0:

```text
backward-compatible fixes              -> patch
backward-compatible public additions   -> minor
breaking public changes                -> major
```

Internal protocol cleanup, HC2 implementation refactors, worker plumbing, and
test harness changes are not public SemVer API unless this document explicitly
classifies them as public.

## Interface Inventory

### Public-Stable

These surfaces are supported for 1.0 users:

- Host CLI: `lmux status`, `lmux term`, `lmux put`, `lmux get`,
  `lmux deploy`, `lmux stream`, `lmux boot info`, `lmux boot worker-status`,
  `lmux boot update-status`, `lmux --help`, and `lmux --version`.
- `lmux stream [--control SOCKET] -- /absolute/program [ARGS...]`.
- Target supervisor launcher mode:
  `lmuxd --supervisor --outer --carrier PATH [--storage-root DIR] [--worker-path PATH] [--verbose]`.
- Host daemon mode:
  `lmuxd --role host --outer --carrier PATH --control PATH --structured-control PATH ...`.
- OUTER/1 framing and BOOT management as documented in `outer1-wire.md`.
- LMUX/2 framing and service reconciliation as documented in `lmux2-wire.md`.
- LXF1 FILE PUT/GET application protocol as used by `lmux put` and `lmux get`.
- PTY console service exposed through `/run/lmux/console1.sock`,
  `/run/lmux/console2.sock`, and `lmux term`.
- Unix rendezvous byte bridge, including the default legacy GDB rendezvous
  socket `/run/lmux/gdb.sock`.
- Worker deployment through BOOT-managed worker state.
- Zeromux explicit `probe`, `send`, and distinct-carrier `install` commands as
  experimental bootstrap tooling.
- `lmux-mcp` tool names and argument-level behavior documented in
  `lmux-mcp.md`, with GDB attach as the supported 1.0 debug workflow.

## Document Classification

```text
Current contract:
  README.md
  docs/release-1.0.md
  docs/outer1-wire.md
  docs/lmux2-wire.md
  docs/host-control.md
  docs/lmux-mcp.md
  docs/debugging.md
  docs/zeromux.md

Architecture explanation:
  docs/private-exec.md
  docs/mcp.md

Historical/stale material:
  none intentionally authoritative for 1.0; older milestone labels in
  architecture notes are background only when they conflict with this file.
```

### Internal-Stable

These are stable enough for LMUX components in this repository but are not
long-term external third-party APIs:

- HC2 structured local control socket `/run/lmux/control-v2.sock`.
- Legacy text control socket `/run/lmux/control.sock`.
- Private EXEC service, exec incarnation, and exec refs.
- Supervisor-worker LMS1 link and its barrier semantics.
- BOOT storage layout:
  `/root/lmux/workers/<sha256>.lmuxd` and `/root/lmux/worker.state`.
- `lmux-mcp` runtime layout under `$XDG_RUNTIME_DIR/lmux/mcp/`.

### Internal-Implementation

These may change without API notice:

- C struct layout, enum numeric values not serialized on public wire, buffer
  sizes except where documented as wire/resource bounds, log wording, test-only
  carriers, generated build-number file, and local harness scripts.
- Worker launcher fds, `--worker`, `--carrier-fd`,
  `--supervisor-control-fd`, `--carrier-listen`, and `--carrier-connect`.
- Local temporary names used during FILE, BOOT, and Zeromux transfers.

### Experimental / Do-Not-Promise

These are present but not promised as primary 1.0 public workflows:

- `debug_start mode=launch`.
- `lmux zeromux install` as anything beyond explicit artifact install plus
  distinct-carrier supervisor launch.
- Low-level `lmux boot upload`, `activate`, `confirm`, `rollback`,
  `supervisor-install`, and `supervisor-restart` administration commands.
- `lmux exec SOCKET -- PROGRAM [ARGS...]`, which is a local socket-to-child
  helper and not target private EXEC.

## CLI Contract

`lmux status` prints human status to stdout and returns nonzero on local/control
failure. `lmux term` attaches to PTY console 1 by default; `lmux term 2`
attaches to console 2. PTY semantics are distinct from generic streaming:
terminal modes, window size, and Ctrl-T local commands belong to `term`.

`lmux put` and `lmux get` use `/run/lmux/file.sock` by default and keep progress
on stderr so destination data is not contaminated. FILE transfers verify SHA256.

`lmux deploy FILE` deploys a worker artifact through the target supervisor. It
does not promise supervisor self-update without the documented
`--restart-supervisor` flow and an external lifecycle owner.

`lmux stream` stdout is target stdout payload only. Diagnostics and control
errors go to stderr. On normal target completion, the CLI exits with the target
exit status or `128 + signal`. On LMUX/control/transport failure, it exits with
a local failure status and does not invent a target exit code.

## Wire Contract

LMUX Wire 1.0 uses LMUX/2 only. There is no LMUX/1 fallback or autodetection.

The lifetime separation is part of the architecture:

```text
carrier
!= OUTER session
!= supervisor-worker link
!= LMUX session
!= service
!= execution
!= stream
!= local endpoint
!= PTY process
```

OUTER/1 owns physical framing, exact ordered opaque LMUX byte transport, BOOT
management, endpoint incarnation IDs, `HELLO` / `HELLO_ACK`, CRC32C, and
session failure on current-session corruption.

LMUX/2 owns service reconciliation and stream transport. Stream IDs are
session-local and transient. After reconnect, old streams are terminal and a
new stream namespace is used. Same-worker reconnect may preserve services and
private executions, but it uses fresh transient stream identities.

The stream-ordering invariant fixed before 1.0 is part of the release contract:

```text
logical stdin EOF remains associated with the correct ordered stdin stream
within an LMUX/private-exec incarnation.
```

`DATA` ordering and `STREAM_CLOSE` ordering are per stream. A `STREAM_CLOSE`
does not reorder prior accepted DATA for that stream. Stale DATA for an unknown
or terminal old stream cannot revive a stream.

## MCP and GDB

The 1.0 MCP surface is the `lmux-mcp` tool set documented in `lmux-mcp.md`.
The supported hardware debug workflow is attach:

```text
debug_start mode=attach pid=TARGET_PID symbols=HOST_PATH
```

Hardware-verified attach coverage includes attach/detach, repeated attach,
breakpoints, continue to breakpoint hit, backtrace, evaluate, registers, memory
read, `stepi`, `nexti`, and `debug_stop` cleanup.

`debug_start mode=launch` remains exposed by the schema for compatibility and
software testing, but it is not the 1.0 flagship hardware-supported workflow.

The Hantek target stock debugger is `gdbserver 7.11.1`. Remote Ctrl-C /
`debug_interrupt` is correctly issued by the host, but the vendor gdbserver
does not reliably stop the inferior on raw RSP `0x03`. A successful
`debug_interrupt` return means the request was accepted/sent; callers must
observe `STOPPED` with `debug_wait` or `debug_status`. This is a target
debugger limitation, not an LMUX transport failure.

## Generic Stream Contract

Public:

```text
lmux stream [--control SOCKET] -- /absolute/program [ARGS...]
```

Supported semantics:

- binary-clean stdin/stdout/stderr;
- EOF propagation;
- backpressure through bounded queues and LMUX flow control;
- foreground ownership;
- target exit propagation on normal completion;
- same-worker reconnect;
- cleanup on local pipeline close and signals.

Not supported by `lmux stream`:

- PTY semantics;
- FILE semantics or hashes;
- background jobs or public exec refs;
- shell interpretation;
- PATH lookup.

Use `/bin/sh -c ...` explicitly when shell behavior is wanted.

## FILE Durability Contract

PUT/GET preserve hash verification. Target PUT writes a temporary file, fsyncs
and closes it using the existing policy, verifies size/SHA256, then renames on
success. Bad hash, disconnect, or protocol failure aborts the session and
removes the temporary file best-effort.

The 1.0 durability boundary is atomic replacement across process or transport
interruption after a successful final rename boundary. LMUX Wire 1.0 does not
promise power-loss durability for FILE destination parent directories because
FILE PUT does not fsync the parent directory.

## Known Limitations

- One active HC2 structured local client is supported. A second structured
  client is rejected or closed; legacy control clients are separate.
- GDB attach is the hardware-supported debug workflow; launch is not the
  primary Hantek 1.0 workflow.
- Hantek vendor `gdbserver 7.11.1` does not reliably honor remote Ctrl-C over
  raw RSP `0x03`; observe stop state explicitly after `debug_interrupt`.
- FILE PUT does not promise power-loss durable directory rename.
- No UART relay or nested LMUX in 1.0.
- No target supervisor self-update loop beyond the current BOOT
  install/restart management and an external lifecycle owner.
- No public background generic exec jobs or public exec job-control CLI.
- HC2 and `control-v2.sock` are trusted local APIs. The default host policy
  intentionally uses mode `0777` under `/run/lmux` when root-owned `lmuxd`
  serves normal-user tools; do not expose these sockets as network services.
- BOOT SHA256 is integrity/identity over the trusted physical/control path, not
  authenticity or authorization.

## Support Matrix

```text
Feature                                      1.0 status
---------------------------------------------------------------------------
OUTER/1 USB carrier                          supported
LMUX/2                                       supported
PTY console                                  supported
FILE PUT/GET                                 supported
Unix rendezvous                              supported
Structured private EXEC                      internal-stable
lmux stream                                  supported
Worker deploy                                supported
Supervisor install/restart via deploy        supported with external owner
GDB attach via MCP                           supported / hardware verified
GDB launch via MCP                           experimental / not primary HW flow
Remote Ctrl-C on stock gdbserver             target limitation
HC2 control-v2.sock framing                  internal-stable, not public API
MCP command/file/debug tools                 public tool surface
Zeromux probe/send                           experimental bootstrap tooling
Zeromux distinct-carrier install             experimental bootstrap tooling
Zeromux same-channel handoff                 deferred
UART relay                                   deferred
Nested LMUX                                  deferred
Go host rewrite                              deferred
```

## Test Coverage Matrix

```text
Release guarantee                            Coverage
---------------------------------------------------------------------------
OUTER corruption recovery                    unit: test_link; process integration: supervisor
Carrier reconnect                            process integration: lmux2/supervisor
Worker replacement/reconciliation            process integration: supervisor
PTY persistence/reconnect                    process integration: lmux2_pty
FILE interruption atomicity                   process integration: lmux2_file
Unix rendezvous abandonment                  process integration: lmux2
Structured exec lifecycle                    process integration: supervisor; Go MCP integration
Generic streaming                            process integration: lmux_client/supervisor
Deploy / BOOT worker lifecycle               process integration: supervisor
Zeromux probe/send/install components        unit: test_zeromux
GDB tool surface                             Go unit tests with fake GDB
GDB attach workflow                          hardware verified in P11
```

## Software Release Gate

Run from the repository root:

```sh
sh -n start-host.sh
sh -n lmux-start-target.sh
sh -n scripts/gen-version.sh

git diff --check

make clean
make host
make selftest

GOCACHE=/mnt/linfiles/SourceCode/lmux/build/host/go-cache \
go test ./...

make target
make strip-target
```

After all fixes, `make selftest` must pass at least five consecutive times
without `make clean` between runs. Then perform at least one full clean gate.
Also verify representative help/version output and final `git status`.

## Hardware Acceptance Checklist

Do not run this automatically during P15. The operator should run it on the
target hardware after software gate passes:

```text
1. status/reconciliation
   Procedure: start target supervisor, run start-host.sh, then lmux status.
   PASS: OUTER and LMUX established; console1, console2, gdb, files ready.

2. PTY console
   Procedure: lmux term; run `uname -a`, detach with Ctrl-T q.
   PASS: command output appears, detach returns cleanly, reconnect preserves shell.

3. FILE PUT/GET with hash
   Procedure: put a host test file to /tmp, get it back, compare sha256.
   PASS: source and returned hashes match; no progress text in file data.

4. Generic stream binary exact
   Procedure: pipe deterministic binary bytes through an absolute target command.
   PASS: stdout bytes are exact and diagnostics stay on stderr.

5. Generic stream USB reconnect
   Procedure: run a long stream, briefly interrupt/recover USB carrier.
   PASS: same-worker reconnect resumes or fails with a clear transport error;
   no fake target status is reported.

6. Deploy
   Procedure: lmux deploy candidate lmuxd; then lmux status.
   PASS: candidate is verified/confirmed and services reconcile.

7. GDB attach/breakpoint/detach
   Procedure: use MCP debug_start attach, set breakpoint, continue to hit,
   inspect backtrace/registers/memory, then debug_detach or debug_stop.
   PASS: attach workflow succeeds. Do not require Ctrl-C success on stock
   gdbserver.
```

## Deferred Post-1.0 Work

- UART relay.
- Nested LMUX.
- Zeromux same-channel handoff and fuller bootstrap profiles.
- Rich host frontend.
- Standalone richer MCP UX.
- Go host rewrite.
- FILE auto-window/performance tuning.
- Advanced BOOT update workflow.
- Supervisor replacement/update beyond the current BOOT/deploy scope.
