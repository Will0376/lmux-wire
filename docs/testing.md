# Software test suites

LMUX tests are split by feedback time and subsystem.  The split is only an
execution interface: it does not replace test executables, duplicate their
assertions, or reduce the historical `make selftest` coverage.

All targets in this document are software-only.  They use local processes,
Unix sockets/socketpairs, PTYs, FIFOs, and temporary files.  They do not open
the configured production lmuxd sockets or physical carrier devices.

## Inventory

The following timings were measured on the B2.1 development host on
2026-10-23.  They are wall-clock observations, not timeout budgets.  A warm
cached run will differ from a clean build and from another machine.

| Existing suite | Kind and coverage | Production subsystems | Build needed | Process/resources and isolation | Focused selector | Measured execution |
| --- | --- | --- | --- | --- | --- | ---: |
| `test_protocol` | C unit: CRC-16, COBS, SHA-256, LXF record bounds | framing, LXF | host C | none | executable | 0.001 s |
| `test_lmux2` | C unit: LMUX/2 codec, handshake, streams, flow control, limits and stale traffic | LMUX/2 | host C | in-process connection pair | executable | 0.004 s |
| `test_carrier` | C unit: fd/unix/TTY carrier, partial I/O, reconnect, names, LMC1 codec and RAW demux | carrier, strict TTY, LMC1 | host C | unique `/tmp/lmux-carrier-test-*`, Unix socket, pseudo-TTY | executable | 0.001 s |
| `test_dirty` | C unit: DIRTY record codec, parser, CRCs, fragmentation, windows, ordering, retransmit, wrap and failure bounds | DIRTY carrier adapter | host C | in-process deterministic virtual time | executable | measured in the B3 report |
| `test_dirty_integration` | process integration: two adapters through a fault-injecting Unix proxy | DIRTY adapter, partial I/O and backpressure | host C | forked peers and private socketpairs; no device paths | executable / `make test-dirty` | measured in the B3 report |
| `test_dispatcher` | C unit: dispatcher IPC envelope, bounded C1/D1/escape parser and termios output transform | UART dispatcher | host C | in-process | executable / `make test-dispatcher` | measured in B4.1 report |
| `test_dispatcher_integration` | process integration: real dispatcher on PTY with `OPOST/ONLCR`, exact physical records and console restoration | UART dispatcher, TTY lifecycle | host C | private PTY and `SOCK_SEQPACKET` socket | executable / `make test-dispatcher` | measured in B4.1 report |
| `test_supervisor` | C unit: LMS1 message codec, proxy rings, fd hygiene and storage helpers | supervisor helpers, storage | host C | fork/pipe and unique temp directory | executable | 0.001 s |
| `test_link` | C unit: CRC32C, OUTER golden vectors, HELLO, sequence/error recovery and BOOT interleave | OUTER/1, BOOT | host C | in-process endpoints | executable | 0.001 s |
| `test_zeromux` | C unit/component: probe, marker parsing, sender/install/error paths | Zeromux/bootstrap | host C | child shell, pipes/socketpairs | executable | 5.634 s |
| `test_lmux_client` | C unit: terminal filter, modal commands, paths and progress helpers | `lmux` client | host C | captured local fds; failed local socket connects only | executable | 0.001 s |
| `go test ./...` | Go unit/component: HC2, MCP application, GDB/MI and bounded RSP/debug paths | HC2, MCP, GDB | Go host cache | `t.TempDir`, fake GDB subprocesses and local Unix sockets | Go package or `-run` | 0.214 s warm/cached |
| `test_lmux2_integration` | process integration: host/target LMUX, endpoint exchange, delayed peer/reconnect | LMUX/2, generic streams | host binaries | unique temp directory and Unix test carrier | executable | 56.870 s |
| `test_lmux2_pty_integration` | process integration: PTY durability, detach/reconnect, flow and cleanup | PTY, LMUX/2 | host binaries | unique temp directory, Unix sockets and FIFOs | executable | 7.557 s |
| `test_lmux2_file_integration` | process integration: FILE transfers, abort/reconnect/commit and CLI | FILE, LMUX/2 | host binaries | unique temp directory, Unix sockets and files | executable | 2.640 s |
| `test_supervisor_integration` default | full process regression: carrier, OUTER, worker, BOOT/deploy, services, recovery and in-suite soak loops | supervisor/worker and all host services | host binaries | each scenario uses `mkdtemp`; local Unix carriers and child processes | executable | 337.961 s to the recorded pre-change stream timeout; see Harness findings |
| supervisor carrier B1 filter | named endpoints, strict TTY/standby, missing primary/standby and status | carrier manager, supervisor | host binaries | unique temp resources and two Unix test carriers where needed | `LMUX_SUP_IT_ONLY_CARRIER_B1=1` | 2.295 s |
| supervisor carrier B2 filter | LMC1, RAW round-trip/recovery, ambiguous COMMIT, old-link loss, BUSY/unavailable | carrier, LMC1, OUTER/LMUX reconciliation | host binaries | unique temp resources and independent Unix test carriers | `LMUX_SUP_IT_ONLY_CARRIER_B2=1` | 5.991 s |
| supervisor carrier B4 filter | DIRTY bootstrap corruption, lost COMMITTED/QUERY recovery, console noise inside DATA, lost ACK/retransmit, explicit retry failure | DIRTY carrier handover, OUTER/LMUX reconciliation | host binaries | real host/supervisor/worker and deterministic private socketpair proxy | `LMUX_SUP_IT_ONLY_CARRIER_B4=1` | measured in the B4 report |
| supervisor lifecycle filters | first-contact loss, OUTER fault, reconnect, worker replacement, deploy and install/restart | OUTER, supervisor, worker, BOOT | host binaries | real local processes and unique Unix carriers | `LMUX_SUP_IT_ONLY_*` selectors used by `test-lifecycle` | 3.336 s for isolated structured reconnect |
| supervisor service filters | private/structured EXEC and generic stream basic/cleanup/reconnect | EXEC, HC2, streams | host binaries | real local processes and unique Unix sockets | `LMUX_SUP_IT_ONLY_*` selectors used by `test-services` | 49.248 s for isolated private EXEC; 8.881 s for isolated stream basic |
| MCP process integration | real lmuxd supervisor/host plus MCP stdio client, EXEC and FILE calls | MCP, HC2, EXEC, FILE | host binaries and Go | `t.TempDir`, explicit Unix sockets and runtime directory | `LMUX_MCP_IT=1` | 1.794 s isolated |
| lifecycle recovery loop | 20 carrier/worker recovery cycles with PTY and EXEC reconciliation | supervisor, OUTER, worker, PTY, EXEC | host binaries | unique temp resources; repeated real processes | `LMUX_SUP_IT_ONLY_LIFECYCLE_LOOP=1` | 95.647 s |
| EXEC lifecycle loop | repeated start/drain/release and stdin-close cycles, fd leak check | structured EXEC, HC2 | host binaries | unique temp resources; real processes | `LMUX_SUP_IT_ONLY_EXEC_LIFECYCLE=1` | 5.172 s |
| FILE worker torture | worker death during 8 MiB FILE put and fresh transfer after replacement | FILE, worker lifecycle | host binaries | unique temp resources and files | `LMUX_SUP_IT_ONLY_FILE_WORKER=1` | 5.099 s |
| mixed-service soak | 12 mixed PTY/FILE/EXEC/unix cycles across carrier, worker and peer restarts | full service reconciliation | host binaries | unique temp resources and real processes | `LMUX_SUP_IT_ONLY_MIXED_SOAK=1` | 24.057 s |

No test target requires an ARM binary.  ARM cross-builds remain part of release
verification.  The measured clean builds were 29.264 s for `make host` and
10.455 s for `make target`; `make clean` itself took 0.076 s.

Measured target-level results after the split were:

| Target | Result | Observed wall time |
| --- | --- | ---: |
| `make test-unit` | pass, steady-state build and Go cache | 5.849 s |
| `make test-carrier` | pass, warm binaries | 9.307 s |
| `make test-lifecycle` | pass | 33.778 s |
| `make test-services` | pass, including MCP process integration | 203.351 s |
| `make test-soak` | pass; this verification run also rebuilt changed prerequisites | 176.164 s |
| `make verify-release` | pass from clean tree artifacts through stripped ARM binaries | 472.769 s |

The supervisor executable also retains direct opt-in selectors for private
EXEC EPIPE, concurrent EXEC, scheduler behavior, multi-reconnect,
multi-worker replacement, upload/deploy fault injection, stream reconnect,
and composed recovery sequences.  The existing `agent-scripts/repeat-*.sh`
and `agent-scripts/phase1-stability.sh` remain available for larger external
repeat counts.

## Make targets

| Target | Contents | Intended use |
| --- | --- | --- |
| `make test-unit` | all nine C unit executables plus ordinary `go test ./...` | fast local validation; the opt-in MCP process test remains skipped |
| `make test-dirty` | DIRTY unit suite and two-process deterministic fault proxy | shortest DIRTY framing/reliability loop |
| `make test-dispatcher` | dispatcher unit/PTY tests plus real host/supervisor/worker DIRTY handover and emergency console CLI | console-aware UART development |
| `make test-carrier` | carrier and DIRTY unit suites, adapter fault proxy, B1 named endpoint/standby, B2 RAW and B4 DIRTY handover process groups | normal carrier development |
| `make test-carrier-switch` | B2 RAW plus B4 DIRTY switch/recovery process groups | quickest process-level switch/recovery check |
| `make test-carrier-soak` | adapter fault proxy and B2/B4 switch groups repeated `CARRIER_SOAK_RUNS` times (default 5) | repeated carrier recovery qualification |
| `make test-lifecycle` | supervisor unit plus first-contact loss, OUTER fault, reconnect, worker replacement, deploy, and install/restart process checks | supervisor/OUTER/worker changes without unrelated service suites |
| `make test-services` | LMUX generic, PTY and FILE process suites; private/structured EXEC; stream basic/cleanup; GDB/HC2 Go tests; opt-in MCP process integration | service, HC2, MCP and debugging work |
| `make test-soak` | repeated carrier switching plus EPIPE/concurrent/multi-session, lifecycle, EXEC, FILE, mixed-service and Go RSP backpressure stress | milestone and pre-acceptance stress; default `SOAK_RUNS=1` because individual scenarios contain their own loops |
| `make selftest` | the historical complete default C/Go suite and version consistency checks | unchanged compatibility/full default gate |
| `make verify-release` | canonical clean software release gate | fail-fast release/milestone verification, without hardware |

`SOAK_RUNS` and `CARRIER_SOAK_RUNS` must be positive integers.  Invalid or
zero values fail rather than reporting a false pass.  Larger external repeat
campaigns are intentionally opt-in, for example:

```sh
make test-soak SOAK_RUNS=3 CARRIER_SOAK_RUNS=10
agent-scripts/phase1-stability.sh
```

The opt-in tests are not silently added to `selftest`.  In particular,
`LMUX_MCP_IT`, concurrent/multi-session supervisor selectors, and the external
repeat scripts keep their previous opt-in status.  The default supervisor run
inside `selftest` already includes one execution of its lifecycle, EXEC, FILE,
and mixed-service loop tests.

`verify-release` runs, in order and fail-fast:

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

Recursive Make invocations are sequential.  `selftest` reuses the preceding
host build, and `strip-target` reuses the preceding target build.  The explicit
second Go command is retained because it is part of the canonical gate; it
normally uses the populated build-local Go cache.

## Selection policy

| Change area | Fast checks | Focused process checks | Full gate required |
| --- | --- | --- | --- |
| Carrier framing | `make test-unit` (or `build/host/test_carrier`) | `make test-carrier` | at carrier milestone completion or when OUTER boundaries may change |
| LMC1/switching | carrier unit test | `make test-carrier-switch`; use `test-carrier-soak` for recovery repetition | at switching milestone completion |
| Supervisor lifecycle | supervisor/link/carrier unit tests | `make test-lifecycle` | for cross-subsystem lifecycle milestones |
| FILE/EXEC/PTY | affected C or Go unit/package tests | `make test-services` and relevant lifecycle selector | when lifetime/reconciliation semantics change |
| MCP/HC2 | affected Go package tests | `make test-services` (explicitly enables MCP process integration) | for protocol-facing or release changes |
| Test harness | affected executable/target | run every target whose selection or cleanup changed | when `selftest` or release orchestration changes |
| Release | `make test-unit` is only preliminary | affected integration and required soak targets | `make verify-release` |

During development, select tests by the changed subsystem and expand based on
actual cross-subsystem risk.  A full gate is not required after every file
edit.  At milestone completion, run the affected integration groups and the
necessary recovery repeats.  Before hardware acceptance or release, run a
clean `verify-release` plus the soak/torture groups relevant to the milestone.

For DIRTY carrier development, the minimum normal loop is:

```sh
make test-unit
make test-carrier
```

`make test-dirty` is the shorter adapter-only loop. Its process executable uses
two forked peers and a private Unix socketpair proxy. The proxy deterministically
inserts console garbage (including inside an encoded DATA record), drops a
fragment and an ACK, duplicates a fragment, and splits every read/write. It
compares all 8192 bytes in both directions and separately checks explicit peer
disappearance failure. No B3 target opens a TTY or hardware path.

Use `make test-carrier-switch` while iterating on process handover. The B4
filter uses real host lmuxd, target supervisor and worker processes. Its proxy
corrupts the first bootstrap record, drops COMMITTED, injects console bytes
inside a DIRTY DATA record, drops an ACK, and verifies QUERY recovery,
selective/full retransmission, worker continuity and post-handover structured
EXEC. A second scenario blackholes all DIRTY data and requires an explicit
`committed-but-down` result instead of a shortened live OUTER stream; its
companion closes the candidate immediately after COMMIT and requires the same
authoritative, non-rollback state. Finish a
carrier milestone with `make test-carrier-soak`, `make test-lifecycle`,
`make test-soak`, and `make verify-release`.

## Isolation and failure behavior

Focused targets deliberately run serially.  Process suites create unique
runtime directories with `mkdtemp`/`t.TempDir`, pass explicit carrier and
control socket paths, and clean child processes and temporary files.  They do
not discover or connect to a running production lmuxd.  No target enables
parallel process scenarios; do not add parallel execution until shared fd,
socket, and child cleanup have been audited.

Every test executable is an explicit Make prerequisite.  If it is missing,
Make rebuilds it; a missing source/tool or failed build is an error.  Recipe
commands and soak loops propagate the first nonzero status.  A selector count
cannot be zero, so a target cannot pass without actually executing tests.

## Harness finding from the audit

The pre-change full supervisor timing run failed after 337.961 s in
`stream_cli_basic`: the diagnostic reported `in_pos=262144` and
`out_len=262144`, followed by a wait timeout.  The same focused scenario
passed immediately afterward in 8.881 s.  It failed once more after a long
services sequence, then again passed in isolation.  The FILE process suite
also failed its `bulk file does not starve PTY` assertion once and passed an
immediate isolated rerun in 2.610 s.  The first release-gate attempt stopped
on the PTY `continuous producer detaches` assertion with a stream left in
`LOCAL_CLOSED`; the immediate isolated PTY suite passed in 7.413 s, and the
next complete release gate passed.

Two existing Go debug tests exposed asynchronous observation races during the
audit.  `TestDebugSocketPathLifetimeAndEndpoint` read status before the
explicit RSP accept event; the test now waits on the session's `connReady`
event instead of polling timing.  A separate
`TestDebugConnectingRSPCloseThenTargetSelectErrorWins` failure lost its
secondary close diagnostic when target-select error publication won the
race; the focused test subsequently passed 10/10 without changes and remains
an open production-diagnostic ordering issue.

Failed process runs also left some unique `/tmp/lmux-*` directories, although
no failed-run lmuxd, `lmux stream`, or `cat` process remained alive.  Successful
runs use unique paths and did not collide with these directories, but failed
path cleanup should be tightened separately.

No timeout, retry budget, buffer, or production behavior was changed to hide
these findings.  Treat the stream, FILE fairness, PTY detach, target-select
diagnostic, and failed-path cleanup observations as sequence/load-sensitive
issues until independently diagnosed; a later successful full gate
demonstrates gate status but does not erase them.
