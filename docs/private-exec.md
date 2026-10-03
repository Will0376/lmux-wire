# Private Exec Architecture

P7 defines a private target execution primitive for future host-side MCP work.
This is a design document only. It does not add a public CLI command, wire
opcodes, service implementation, tests, or deployment behavior.

## Goals

Private exec provides a worker-owned primitive for:

- process lifecycle;
- stdin;
- stdout;
- stderr;
- explicit signals;
- exit status;
- detach and reattach while the same worker lives.

It is intended for internal/programmatic host APIs and a future MCP frontend.
It is not a human terminal, shell prompt parser, public CLI feature, package
manager, process supervisor, or target-side transcript database.

The design must fit the current LMUX stack:

```text
HOST:
internal API / future MCP
    -> host lmuxd
    -> LMUX/2
    -> OUTER/1
    -> carrier

TARGET:
carrier
    -> supervisor / OUTER
    -> LMS1
    -> worker / LMUX/2
    -> private exec service
    -> child processes
```

The target supervisor remains unaware of private exec. It continues to see only
opaque OUTER TRANSPORT bytes.

## Non-Goals

P7/P8 do not include:

- public `lmux exec TARGET_COMMAND` CLI;
- PTY replacement;
- shell prompt parsing;
- terminal emulation;
- command history UI;
- MCP server;
- MCP tokens, cursors, pagination, or output search;
- target-side transcript persistence;
- persistent job scheduler or cron;
- target process manager;
- containerization;
- privilege separation;
- authentication or RBAC;
- FILE redesign;
- Zeromux;
- relay;
- deployment changes.

The existing `lmux exec SOCKET -- PROGRAM` helper, if present in the CLI, is a
local socket-to-child utility. It is not the target private exec service
described here.

## Lifetime Model

Existing LMUX lifetimes remain separate, and private exec adds one more:

```text
factory != service != execution != stream
```

An execution is a durable runtime object inside one worker incarnation:

```text
worker
  |
  +-- private exec service
        |
        +-- execution A
        |     |
        |     +-- stdin attachment stream
        |     +-- stdout attachment stream
        |     +-- stderr attachment stream
        |
        +-- execution B
```

Preferred semantics:

```text
local/MCP disconnect != kill process
LMUX stream detach != kill process
OUTER/LMUX reconnect while same worker lives != kill process
worker death/replacement => execution lost
explicit execution termination => signal/kill according to request
```

This is intentionally closer to the PTY service durability model than to a
single RPC request/response. Long-running commands may run for minutes or hours.

Important lifecycle cases:

- **MCP/local client disconnect:** host presentation detaches. The host may keep
  the LMUX attachment open for policy reasons, or detach from the target
  execution. The target process is not killed solely because the local client
  disappeared.
- **LMUX stream close:** closes that attachment only. It does not imply child
  stdin EOF unless an explicit `CLOSE_STDIN` operation has been accepted.
- **LMUX connection recreation:** all LMUX streams are gone. Executions survive
  if the same worker survives. The host must re-discover/reconcile the exec
  service, read the service incarnation, and attach again by `exec_id`.
- **OUTER reconnect:** same as LMUX reconnect from the worker service point of
  view. OUTER is below LMUX and must not leak into exec semantics.
- **Worker crash/replacement:** all executions are gone. Old `exec_id` values
  are stale and must never attach to a new execution in the new worker.
- **Explicit termination:** a `SIGNAL` operation requests delivery to the
  execution process group. `SIGKILL` is the final forced cleanup mechanism.

## Execution Identity

Each execution has a nonzero `u64 exec_id`.

Rules:

- unique within one worker incarnation;
- monotonically allocated or otherwise guaranteed never to repeat in that
  incarnation;
- no wraparound reuse: exhaustion makes new starts fail until worker restart;
- stale after worker replacement;
- not an MCP token;
- not a LMUX `stream_id`;
- not a LMUX `service_id`;
- not persistent across target reboot or worker replacement.

The exec service exposes a nonzero random `u64 exec_incarnation` generated once
when the worker-owned service is initialized. It is not persistent and is in a
separate namespace from OUTER connection IDs, LMUX connection IDs, LMUX peer
restart counters, LMS1 epochs, stream IDs, and MCP command tokens.

```text
same worker + fresh OUTER/LMUX => same exec_incarnation
worker death/replacement       => new exec_incarnation
```

Host state must bind every `exec_id` to the current `exec_incarnation`. After
worker replacement, all old handles are invalid even if a future worker
allocates the same numeric value. P8 should avoid numeric reuse within an
incarnation so stale-handle bugs are easier to catch.

## Process Model

P8 should use the existing single-process, single-`poll()` style:

```text
fork
setpgid
pipe stdin/stdout/stderr
dup2 pipes in child
optional chdir
execve
parent sets nonblocking fds
parent polls child pipes and LMUX streams
waitpid(WNOHANG)
```

No threads are introduced.

The child should run in a separate process group. Both child and parent should
attempt `setpgid(child, child)` around the fork/exec boundary to close the usual
race. Signals are then sent to the process group, not merely the leader PID.

Implementation should use a close-on-exec error pipe or equivalent to distinguish
early `chdir`/`execve` failure from a normal program that starts and exits
quickly.

## Start Semantics

Private exec starts a direct process. The basic request is:

```text
argv[]
optional cwd
```

Example:

```text
["/bin/ls", "-la", "/tmp"]
```

There is no implicit shell. If the caller wants shell semantics, it must ask for
them explicitly:

```text
["/bin/sh", "-c", "printf '%s\n' \"$HOME\" | sed 1q"]
```

This keeps heredocs, pipes, shell expansion, redirection, quoting, and multiline
scripts in the caller's chosen shell rather than in the LMUX exec protocol. The
exec protocol does not know what a heredoc is.

For v1, `argv[0]` should be an absolute executable path. PATH lookup is deferred.
If the caller wants PATH search, it can explicitly invoke `/bin/sh -c` or a
known wrapper.

The argv list is bounded. P8 should define concrete limits during wire work,
but a reasonable starting point is:

```text
argv entries:       16
total argv bytes:   4096
single argv bytes:  bounded by the service record limit
```

All strings are byte strings with explicit lengths on the wire. No serialized C
structs and no peer-sized malloc.

## Environment

V1 uses the worker process environment unchanged and does not support explicit
per-execution environment variables.

Reasons:

- keeps the P8 surface small;
- avoids large request bodies;
- avoids environment merge/delete semantics;
- lets callers use `/bin/sh -c 'VAR=value command'` when they truly need shell
  environment behavior.

Future versions may add an explicit bounded environment list. An empty/minimal
environment mode is also deferred.

## Cwd

The cwd is optional:

- absent cwd: inherit the worker's current working directory;
- present cwd: must be an absolute target path in v1;
- relative cwd: rejected;
- invalid or inaccessible cwd: execution start failure;
- cwd is applied before `execve`.

Private exec cwd is independent of FILE service cwd helpers and PTY shell cwd.
Those are separate service/application states.

When the caller uses `/bin/sh -c`, the shell runs with the selected cwd just like
any other child process. Shell expansion does not change the LMUX exec cwd
rules.

## Stdio Semantics

Private exec has three logical byte-clean channels:

```text
stdin
stdout
stderr
```

stdout and stderr are not merged by default.

Rules:

- stdin bytes are delivered to the child stdin pipe in order.
- `CLOSE_STDIN` closes the child stdin pipe after all accepted stdin bytes have
  been written.
- Detaching or losing the stdin attachment stream does not by itself close child
  stdin.
- stdout and stderr bytes are delivered exactly in per-channel order.
- stdout and stderr may reach EOF independently.
- Child process exit does not discard already-produced stdout/stderr.
- An execution is not fully done until the process has been reaped and both
  stdout and stderr EOF have been observed and made available to the host.

The important invariant is:

```text
exit status must not cause already-produced stdout/stderr bytes to be lost
```

If the child exits after writing output, the worker remains in an
`EXITED_DRAINING` equivalent state until output pipes drain and EOF is delivered
or the execution is forcibly released by policy.

## Stream And Attachment Model

Preferred design: a service-specific control stream plus separate LMUX streams
for stdin, stdout, and stderr.

```text
exec control stream:
    START
    ATTACH
    SIGNAL
    CLOSE_STDIN
    STATUS
    RELEASE
    state/errors

stdin stream:
    raw stdin bytes host -> target

stdout stream:
    raw stdout bytes target -> host

stderr stream:
    raw stderr bytes target -> host
```

This is preferred over a single multiplexed stream because it reuses existing
LMUX stream flow control and keeps stdout/stderr backpressure independent. It
also avoids creating another transport inside LMUX DATA.

It is preferred over target-side spooling because the target baseline has about
55 MiB RAM and no swap, and persistent filesystem policy does not belong in the
worker exec primitive.

P8a uses `STREAM_OPEN` tokens for service-specific role binding rather than a
second handshake on the control stream:

```text
token = role << 56 | exec_id

role:
    0 or CONTROL   control stream
    STDIN          raw stdin bytes
    STDOUT         raw stdout bytes
    STDERR         raw stderr bytes
```

The token is the existing LMUX/2 little-endian `u64` stream-open token. P8a/P8b
reserve the high 8 bits for the stream role and the low 56 bits for `exec_id`.
This makes the effective `exec_id` namespace 56 bits within one worker
incarnation. Allocation failure on overflow is preferable to reuse. The id is
still runtime-local, nonpersistent, and not an MCP token. The service must not
reuse an `exec_id` within the same worker incarnation.

The P8a control stream carries fixed service records:

```text
u8  type
u8  status_or_error
u16 payload_len
u64 exec_id
bytes payload
```

Integers are little-endian. Payloads are operation-specific byte strings, not
serialized C structs.

P8a record types:

```text
INFO
START
SIGNAL
CLOSE_STDIN
STATUS
RELEASE

INFO_REPLY
START_REPLY
STATUS_REPLY
ERROR
```

`INFO_REPLY` returns:

```text
u64 exec_incarnation
```

encoded little-endian. The host must obtain this value after exec service
reconciliation before deciding whether old execution references are eligible
for automatic reattach. A mismatch means worker replacement, not a recoverable
LMUX reconnect.

P8a `START` payload:

```text
u16 cwd_len
u8  argc
u8  reserved = 0
u16 argv_len[argc]
bytes cwd
bytes argv[0]
...
bytes argv[argc - 1]
```

`argv[0]` must be an absolute target path. `cwd`, when present, must also be an
absolute target path. The environment is inherited from the worker process in
P8a; explicit environment mutation is deferred.

`CLOSE_STDIN` is the authoritative EOF operation. Host-side P8a plumbing only
queues it after already accepted stdin stream bytes have drained from the local
LMUX stream TX buffer, so child stdin sees data-before-EOF. This avoids relying
on cross-stream luck while still keeping stdin bytes on their own LMUX stream.

### Single Active Attachment

V1 supports one active attachment per execution.

If a second host attachment arrives while one is active, the service returns
`BUSY`. Takeover semantics are deferred because they require careful ownership
and data-loss rules.

### Attachment Loss

When the active attachment disappears:

- execution remains alive;
- stdin pipe remains open unless `CLOSE_STDIN` was accepted;
- stdout/stderr are no longer drained beyond bounded local buffers;
- host can later attach to the same `exec_id` if the same worker still owns it.

## Signals And Process Groups

Signals are explicit control operations, not data bytes:

```text
EXEC_SIGNAL(exec_id, signal)
```

V1 should support a small named set:

```text
SIGINT
SIGTERM
SIGKILL
SIGHUP
```

Arbitrary signal numbers are deferred.

Signals are delivered to the execution process group:

```text
kill(-pgid, signal)
```

This is required for shell pipelines:

```text
/bin/sh -c 'foo | bar | baz'
```

The whole pipeline should receive Ctrl-C style interruption. A byte `0x03` on
stdin is not signal delivery for private exec.

Expected edge behavior:

- `ESRCH` after the process has exited is a semantic `NOT_RUNNING` or
  `ALREADY_EXITED`, not a LMUX/OUTER failure.
- stale `exec_id` returns a semantic error.
- signal after `DONE` is a semantic error.
- `SIGKILL` may be used for final cleanup, but normal explicit termination
  should prefer `SIGTERM` or `SIGINT` according to host policy.

No PTY is introduced for signal delivery.

## Detach And Reconnect Policy

Chosen v1 policy:

```text
execution survives detach;
output while detached is preserved only up to bounded buffers;
after buffers fill, child blocks naturally on its stdout/stderr pipe.
```

This policy preserves exact bytes and bounded RAM. Its cost is that a detached
process that produces enough output may stall until the host reattaches or
signals it.

Rejected for v1:

- dropping output while detached, because programmatic exec should not silently
  lose stdout/stderr;
- target-side transcript spooling, because it adds storage lifecycle and cleanup
  policy to the constrained target.

The future MCP layer may keep a host-side transcript under `/run/lmux` or a
bounded local spool. That is outside the target exec service.

## Backpressure And Bounded Memory

The worker must never read unbounded child output into userspace memory.

Output path:

```text
child stdout/stderr pipe
    -> small per-execution userspace buffer
    -> LMUX stream TX ring / flow control
    -> host
```

If the corresponding LMUX stream cannot accept data, the worker stops reading
that child fd. The kernel pipe then backpressures the child.

Input path:

```text
host
    -> LMUX stdin stream
    -> small per-execution userspace buffer
    -> child stdin pipe
```

If the child stdin pipe cannot accept data, the worker stops consuming more
stdin stream bytes. LMUX flow control then backpressures the host.

Temporary `EAGAIN`, partial writes, and full rings are backpressure, not service
failure. Process death, bad service records, invalid exec IDs, or local fd
errors are service-level events/errors and should not reset OUTER or unrelated
LMUX streams.

Initial P8 buffer guidance:

```text
stdin adapter buffer per execution:  <= 1024 bytes
stdout adapter buffer per execution: <= 1024 bytes
stderr adapter buffer per execution: <= 1024 bytes
```

The exact constants may follow existing LMUX stream ring sizes, but increasing
buffers must not be used as a substitute for correct backpressure.

## Completion And Status

Recommended execution states:

```text
STARTING
RUNNING
EXITED_DRAINING
DONE
START_FAILED
KILLED_OR_TERMINATING
```

The exact enum names are not important, but the state model must distinguish:

```text
process has exited
```

from:

```text
all stdout/stderr EOF and final status have been delivered
```

Status should report:

- current state;
- `exec_id`;
- whether stdin is open or closed;
- whether stdout/stderr reached EOF;
- normal exit code, when exited normally;
- terminating signal, when signaled;
- start failure reason, when available;
- whether an attachment is active.

`DONE` means:

```text
process reaped
stdout EOF observed
stderr EOF observed
final status available
```

DONE metadata is retained only according to the bounded retention policy below.

## Resource Limits And GC

P8 should use fixed bounds suitable for the current target.

Recommended initial limits:

```text
max running executions:             4
max retained completed executions:  8
max active attachment per execution: 1
argv entries:                       16
total argv bytes:                   4096
per-channel adapter buffer:         about 1024 bytes
```

Completed execution metadata is not command history. It should be retained only
long enough for the host/MCP layer to observe final status.

Recommended GC policy:

```text
DONE execution retained until RELEASE
or until completed-retention limit forces oldest unattached DONE eviction
```

Eviction makes that `exec_id` stale. Because IDs are not reused within the
worker incarnation, eviction cannot accidentally attach to a different
execution.

Running executions are never evicted by metadata GC. If the running limit is
reached, START returns `LIMIT`.

## Security And Exposure

LMUX currently assumes a trusted host. Private exec grants arbitrary target
process execution with the worker's privileges.

That is why P8 must not expose this service through a casual public CLI or an
unprotected generic local Unix socket. The first consumer should be an internal
host API and later an intentionally designed MCP frontend.

Authentication, RBAC, per-command authorization, and audit policy are future
host/product concerns. They are not target wire semantics.

## Internal Host API Boundary

The host daemon should expose a private internal API roughly shaped like:

```text
exec_start(argv, cwd, options) -> exec_handle
exec_attach(exec_handle) -> attachment
exec_write_stdin(attachment, bytes)
exec_close_stdin(exec_handle)
exec_signal(exec_handle, signal)
exec_status(exec_handle)
exec_release(exec_handle)
```

This is not a public ABI. P8 should keep it narrow enough for the future MCP
frontend and tests, without freezing a long-term application API.

The host API owns:

- mapping worker incarnation + `exec_id` to local handles;
- detecting stale handles after worker replacement;
- reconnecting attachments after LMUX/OUTER recovery;
- storing any optional host-side transcript for later MCP use.

It should not require `control.sock` public commands in P8.

## MCP Boundary

Private exec deliberately does not provide:

```text
session_id
command token
cursor
page 1/10
last 100 lines
output search
persistent transcript
```

Those are MCP/host presentation concepts.

Preferred future shape:

```text
MCP session
   |
   +-- token cmd-1 -> worker incarnation W, exec_id 17
   +-- token cmd-2 -> worker incarnation W, exec_id 18
```

The MCP layer stores:

- stdout/stderr transcript;
- offsets and cursors;
- pagination metadata;
- command display names;
- client-facing session handles.

Cursors should refer to the host transcript, not to target execution protocol
state.

## Worker Restart And Stale Handles

After worker crash or replacement:

```text
all private exec executions are gone
```

Old `exec_id` values become stale. Host state must discard or mark all handles
from the old worker incarnation. It must not attach an old MCP command token to
a new worker execution.

There is no persistent execution recovery in P8. If a command must survive
worker replacement, that is a later process-manager feature, not private exec.

## Source Placement

Recommended target placement:

```text
src/services/service_exec.c
```

Add a small header only if the implementation needs private structures shared
outside that file. Do not create a directory-per-file subsystem.

Expected generic additions during P8:

- extend `enum v2_service_type` with a private exec service type;
- advertise the exec factory in normal `FACTORY_REPORT`;
- reconcile an internal host desired service through the normal
  `SERVICE_ENSURE` / `SERVICE_OK` path;
- add service-specific control/attachment records;
- integrate exec poll fds into the existing worker poll loop;
- mark the host service as internal-only so no public Unix listener or user
  endpoint is created.

Factory/service naming recommendation:

```text
service type: exec
internal service name: __exec
```

P8b implements private as an API/UX boundary, not as a hidden LMUX lifecycle.
The target sees a normal durable exec service with a normal `service_id`; the
host authoritative desired state includes the internal `__exec` service. The
host does not create a public listener for it, and there is no public `lmux
exec` command. Internal test/programmatic control may use the reconciled service
once it exists, and future MCP code should do the same.

The supervisor, OUTER, BOOT, FILE, PTY, and Unix rendezvous services do not need
semantic changes for P8.

## Wire Placement

Private exec belongs above LMUX/2 as a worker service protocol. It does not
belong in OUTER/1 or BOOT.

Rationale:

- OUTER is physical framing and supervisor management.
- BOOT is supervisor-managed deployment/update control.
- Exec children are worker-owned runtime objects, like PTYs and FILE sessions.
- Supervisor must remain unaware of LMUX services.

P8 should document the service protocol either in `docs/lmux2-wire.md` or a
small companion service document when the exact record encoding is implemented.

## P8 Implementation Scope

P8 should be limited to:

- private exec factory/service;
- execution lifecycle;
- direct absolute-path argv spawn;
- optional absolute cwd;
- inherited worker environment only;
- stdin/stdout/stderr;
- explicit signal operation;
- process-group signal delivery;
- exit status;
- single active attachment;
- detach/reconnect semantics using bounded target buffering;
- resource cleanup;
- internal host API;
- no public target exec CLI.

Deferred:

- explicit environment list;
- PATH lookup;
- transcript/history;
- MCP sessions/tokens/cursors;
- pagination;
- multi-observer attach;
- attach takeover;
- target filesystem spooling;
- persistent executions across worker restart.

## P8 Test Plan

Process basics:

- `/bin/true` exits 0.
- `/bin/false` exits nonzero.
- `/bin/echo` produces stdout and drains before DONE.

stdout/stderr separation:

- child writes both streams;
- host receives them on distinct channels;
- one blocked channel does not merge into the other.

stdin:

- `cat`-like command receives binary data;
- explicit `CLOSE_STDIN` produces child EOF;
- stream detach alone does not close child stdin.

Long-running:

- process remains alive while host polls other services;
- PTY, FILE, and BOOT remain usable.

Signals:

- shell pipeline receives SIGINT as a process group;
- SIGTERM and SIGKILL cleanup work;
- signal after exit returns a semantic error.

Detach/reattach:

- execution survives local client detach;
- execution survives LMUX/OUTER reconnect while worker lives;
- host reattaches to the same `exec_id`;
- second simultaneous attach returns BUSY.

Backpressure:

- large stdout with stopped reader stays bounded;
- child blocks naturally instead of worker RAM growing;
- stdin backpressure stops LMUX consumption when child stdin pipe is full.

Exit drain:

- child exits after producing output;
- all already-produced stdout/stderr is delivered before DONE.

Worker death:

- worker replacement makes old `exec_id` stale;
- host cannot attach old handle to a new worker execution.

Cleanup:

- no pipe fd leak;
- no zombie child;
- no process group leak;
- no stream leak;
- completed metadata GC is bounded and deterministic.

Concurrent executions:

- a small bounded parallel set runs under one poll loop;
- exceeding the running limit returns LIMIT.

Regression:

- PTY, FILE, Unix rendezvous, OUTER, BOOT, deploy, and service reconciliation
  remain green.

## Unresolved Questions

P8a resolves the basic service record encoding and role-token binding. The main
remaining design questions are:

1. The ergonomic host internal API shape for MCP-facing callers.
2. Whether a future explicit `ATTACH` record is needed beyond opening role
   streams with an `exec_id`, once host-side reattach UX is implemented.
3. Final policy for completed metadata retention once MCP transcript ownership
   exists.
