# LMUX Wire

LMUX Wire is a userspace transport and service stack for embedded Linux. It is
designed to give host tooling a durable way to reach a constrained target
without kernel modules, USB networking, `n_gsm`, helper threads, or a second
runtime process per service.

Current high-level shape:

```text
HOST:
lmux / lmux-mcp
    -> local control / service interfaces
    -> lmuxd
    -> LMUX/2
    -> OUTER/1
    -> carrier

TARGET:
carrier
    -> supervisor / OUTER
    -> LMS1
    -> worker / LMUX/2
    -> services
```

The target runtime is intentionally small and event-driven. A target deployment
uses a supervisor process and a worker process; each `lmuxd` process uses one
`poll()` loop and no helper threads. Host-side `lmux-mcp` is a separate Go
process and is not part of that target daemon invariant. LMUX/2 is the active
runtime wire protocol; there is no LMUX/1 fallback or autodetection.

LMUX is primarily a stable substrate for host CLI and MCP tooling. It is not
trying to become an OTA framework, package manager, USB network stack, or
kernel-side mux.

New to LMUX? Start with the [hands-on quick start](docs/quickstart.md):
connect a target, open a shell, transfer a file, run a byte stream, and watch
it survive a physical USB reconnect.

## Target Baseline

The current hardware baseline is the Hantek DSO2D15 class target:

```text
CPU:      ARM926EJ-S / ARMv5TEJ
kernel:   Linux 5.2
libc:     glibc 2.23
userland: BusyBox
memory:   about 55 MiB RAM, no swap
ABI:      EABI5 soft-float
build:    static target binary
```

Typical development carriers are:

```text
target: /dev/ttyGS0
host:   /dev/ttyACM0
```

LMUX is not Hantek-specific; that target is simply the currently tested
low-resource embedded baseline.

## Architecture

Do not conflate the lifetimes in the stack:

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

Physical carrier loss retires the current OUTER/LMUX session, but it does not
necessarily kill the target supervisor, worker process, durable services, or
PTY children. Worker replacement is a separate managed lifecycle event; it
creates a fresh LMUX incarnation and the host reconciles desired services again.

## OUTER/1

OUTER/1 is the physical framing and supervisor-management layer between host
and target supervisor. It uses COBS records with a zero delimiter, CRC32C,
endpoint connection incarnation IDs, and strict established-session
`frame_seq`.

First contact uses explicit `HELLO` / `HELLO_ACK` handshake semantics plus
bounded periodic HELLO retry while negotiating. This matters on real USB serial
links: a locally successful write of an early HELLO does not prove that the
peer endpoint received it. Both target-first and host-first startup are expected
to converge.

OUTER carries two planes:

- `TRANSPORT`: exact opaque LMUX/2 byte chunks.
- `BOOT`: supervisor management messages.

The core transport invariant is:

```text
TRANSPORT = exact ordered LMUX bytes OR explicit OUTER failure
```

A bad CRC, sequence gap, malformed current-session frame, or other
current-session protocol failure retires the OUTER session instead of silently
creating a hole in the LMUX byte stream. See [docs/outer1-wire.md](docs/outer1-wire.md)
for the wire specification.

## Supervisor And Worker

The target supervisor owns:

- the physical carrier;
- OUTER/1;
- BOOT;
- worker process lifecycle;
- LMS1 supervisor-worker link;
- persistent managed-worker state.

The supervisor proxies LMUX bytes as opaque TRANSPORT payload. It does not parse
LMUX streams, services, PTY data, FILE data, private EXEC data, or factory
reports.

The worker owns:

- LMUX/2;
- service factories;
- PTYs;
- FILE;
- Unix rendezvous sockets;
- private structured EXEC.

This boundary keeps transport/lifecycle recovery separate from service-level
protocols.

## Current Services

Built-in worker services:

- `pty`
- `unix-rendezvous`
- `file`
- private structured exec (`__exec`, internal)

Default desired services in the host helper are:

- `console1`
- `console2`
- `gdb`
- `files`
- `__exec` (internal)

### PTY

The PTY service starts `/bin/sh`, supervises the child, applies window-size
updates, and lets `lmux term` attach to a console service. PTY children are
durable across stream detach/reconnect while the worker stays alive.

### Unix Rendezvous

The Unix rendezvous service is a generic binary-clean byte bridge. The default
`gdb` rendezvous socket remains available as a manual/legacy escape hatch, but
the hardware-verified MCP GDB attach path does not depend on it. Current MCP
debugging uses `lmux-mcp`, host GDB/MI, a private RSP bridge, and structured
EXEC to run target `gdbserver` over stdio; `lmuxd` and rendezvous services do
not parse GDB/MI/RSP.

### FILE

The FILE service uses LXF over LMUX for PUT and GET. Transfers include SHA
verification, and target-side replacement uses temporary files, fsync, and
atomic replacement where applicable. This protects replacement across process
or transport interruption after the final rename boundary; it does not claim
power-loss durability for the parent directory. Current FILE behavior is
functional and tested; receive-window autotuning is intentionally not
advertised as a current feature.

### Structured EXEC

The private structured EXEC service is an internal worker service used by
`lmux stream`, `lmux-mcp` command tools, and MCP debugging. It starts direct
target argv executions with byte-clean stdin/stdout/stderr and bounded target
capacity. It is not a public background job-control API, and the legacy
`lmux exec SOCKET -- PROGRAM` helper is only a local socket-to-child utility.

## Build

Common developer build and verification commands:

```sh
make clean
make host
make selftest
make target
make strip-target
```

The usual host outputs are under `build/host/`; static target outputs are under
`build/target/`.

## Quick Start

On the target, run the supervisor in OUTER mode:

```sh
/root/lmux/lmuxd --supervisor --outer --carrier /dev/ttyGS0
```

The supervisor also accepts `--storage-root DIR` for managed worker storage. If
omitted, it uses the directory containing the actual running executable as
resolved through `/proc/self/exe`.

On the host, the helper starts the default topology:

```sh
./start-host.sh
```

It runs roughly:

```sh
build/host/lmuxd \
  --role host \
  --outer \
  --carrier /dev/ttyACM0 \
  --control /run/lmux/control.sock \
  --structured-control /run/lmux/control-v2.sock \
  --pty console1:/run/lmux/console1.sock:/bin/sh \
  --pty console2:/run/lmux/console2.sock:/bin/sh \
  --unix gdb:/run/lmux/gdb.sock:/tmp/lmux-gdb.sock \
  --file files:/run/lmux/file.sock \
  --verbose
```

The internal `__exec` service is reconciled automatically by host `lmuxd` when
the target advertises the exec factory; it is not configured with a separate
startup flag.

Basic commands:

```sh
lmux status
lmux term
lmux term 2
lmux put ./local.bin /tmp/remote.bin
lmux get /tmp/remote.bin ./local.bin
tar -cf - DIR | lmux stream -- /bin/tar -xf - -C /tmp
lmux stream -- /bin/dd if=/dev/device bs=65536 > image.bin
```

`lmux term` uses local Ctrl-T commands:

```text
Ctrl-T ?        help
Ctrl-T q/.      detach
Ctrl-T Ctrl-T   send literal Ctrl-T
Ctrl-T p        upload file   HOST -> TARGET
Ctrl-T g        download file TARGET -> HOST
```

`lmux put` and `lmux get` use `/run/lmux/file.sock` by default. Use
`--remote-dir`, `--channel 1|2`, `--mode OCTAL`, `-S SOCKET`, and `-v` as shown
by `lmux --help`.

`lmux stream -- /absolute/program [ARGS...]` runs one foreground target process
through the structured EXEC path and maps local stdin/stdout/stderr to the
target process byte streams. It is binary-clean, forwards stdin EOF, keeps target
stderr separate from stdout, and exits with the target exit status when the
execution completes normally. The target program path must be absolute; LMUX
does not perform target PATH lookup or interpret shell syntax. Use
`lmux stream -- /bin/sh -c '...'` when a target shell is explicitly desired.

`lmux stream` is not a PTY and does not provide terminal modes, prompts, window
size, FILE hashes, atomic replacement, resume offsets, or background job
handles. For interactive shells use `lmux term`; for verified file replacement
use `lmux put` / `lmux get`. The command currently uses the single structured
control client slot at `/run/lmux/control-v2.sock`, so another structured client
such as `lmux-mcp` may make it fail with a clear BUSY/error instead of stealing
ownership.

The checked-in `lmux-start-target.sh` starts the target supervisor in OUTER/1
mode using `LMUXD` and `LMUX_CARRIER` overrides. It is a simple BusyBox-friendly
launcher, not a persistence policy.

## Managed Deployment

High-level worker deployment is:

```sh
lmux deploy FILE
```

The host daemon owns the orchestration:

```text
upload candidate
-> activate as managed worker
-> wait for fresh LMUX
-> reconcile desired services
-> verify target is still running the candidate
-> confirm
-> install the confirmed ELF as the active supervisor executable
```

The target supervisor owns worker lifecycle. The external system init or
launcher owns supervisor lifecycle.

To apply the installed supervisor immediately:

```sh
lmux deploy FILE --restart-supervisor
```

After successful worker confirmation and supervisor install, the supervisor
cleanly exits. An external lifecycle owner, such as BusyBox `::respawn` or an
equivalent launcher loop, must start the active supervisor executable again.
The host then waits for fresh OUTER/LMUX convergence and service reconciliation.

For an already confirmed and running artifact:

```sh
lmux deploy --restart-supervisor
```

This shortcut skips upload/activation/confirmation, installs the currently
confirmed artifact as the active supervisor executable if needed, and requests a
supervisor restart.

There is no supervisor A/B rollback subsystem, supervisor self-exec, or
package-manager policy here. Persistent `worker.state` is the authoritative
managed worker state, not a general deployment database.

Low-level administration and diagnostics are available under `lmux boot ...`,
including:

```text
lmux boot info
lmux boot worker-status
lmux boot upload FILE
lmux boot activate SHA256
lmux boot update-status
lmux boot confirm ACTIVATION_ID
lmux boot rollback ACTIVATION_ID
lmux boot supervisor-install SHA256
lmux boot supervisor-restart SHA256
```

`BOOT` is not a bootloader. It is the supervisor-management protocol carried
inside OUTER/1.

## Experimental: Zeromux

Zeromux is an experimental host-side bootstrap/recovery subsystem for targets
where LMUX is not already running. It is not part of the normal LMUX transport
path and is currently frozen rather than an active expansion direction.

Implemented pieces include:

- authenticated-shell probe over an explicitly selected UART;
- serial configuration;
- host-side classic uuencode sender to target `uudecode`;
- explicit distinct-carrier install/launch.

Verified hardware work included an authenticated Hantek UART shell and a
1 MiB uuencode transfer with SHA verification. Zeromux remains a bootstrap
experiment/tool; same-channel handoff, profiles, ZMODEM, relay integration, and
automatic expansion are not active roadmap items. See
[docs/zeromux.md](docs/zeromux.md) for details.

## 1.0 Hardware-Verified Status

Hardware-tested on Hantek DSO2D15:

- OUTER/1 plus LMUX/2 establishment and service reconciliation;
- physical USB carrier reconnect;
- PTY `console1` and `console2`;
- FILE PUT/GET with SHA verification;
- managed deploy and supervisor restart through an external lifecycle owner;
- generic binary `lmux stream`;
- `lmux stream` continuation across real USB disconnect/reconnect when the
  worker survives;
- MCP command execution;
- MCP GDB attach, breakpoint, continue-to-hit, inspection, and detach.

The stock Hantek `gdbserver` accepts remote Ctrl-C delivery inconsistently; see
[docs/debugging.md](docs/debugging.md) for the debug interrupt limitation.

## Post-1.0 Deferred Work

1.0 freezes the supported public surface described in
[docs/release-1.0.md](docs/release-1.0.md). Deferred directions are kept
outside the 1.0 scope: UART relay, nested LMUX through a relay, bare-target
relay bootstrap, richer host/frontend/MCP UX, Go host rewrite, automatic
Zeromux expansion, advanced BOOT update workflow, supervisor self-exec, A/B
supervisor rollback, broader supervisor update policy, and near-term FILE
window performance tuning.

## Development Verification

Canonical local verification:

```sh
sh -n start-host.sh
sh -n lmux-start-target.sh
sh -n scripts/gen-version.sh
git diff --check
make clean
make host
make selftest
go test ./...
make target
make strip-target
```

For README-only changes, `git diff --check` is normally sufficient.

## License

LMUX Wire is licensed under the Mozilla Public License 2.0. See
[LICENSE](LICENSE).

Unless otherwise noted, project-authored source code in this repository is
covered by the Mozilla Public License 2.0. Third-party components and
dependencies remain subject to their respective licenses.

## Documents

- [docs/lmux2-wire.md](docs/lmux2-wire.md): LMUX/2 wire protocol.
- [docs/outer1-wire.md](docs/outer1-wire.md): OUTER/1 physical wire protocol
  and BOOT framing.
- [docs/debugging.md](docs/debugging.md): MCP/GDB integration and target
  debugger limitations.
- [docs/lmux-mcp.md](docs/lmux-mcp.md): MCP server, tools, runtime, and
  structured-control integration.
- [docs/zeromux.md](docs/zeromux.md): experimental bootstrap/install subsystem.
- [docs/release-1.0.md](docs/release-1.0.md): supported 1.0 contract,
  limitations, and release gate.
