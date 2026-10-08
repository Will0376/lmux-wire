# P11 MCP Debugging

P11 adds host-side agent debugging through `lmux-mcp` without changing OUTER/1,
LMUX/2, LMS1, target supervisor protocol, or existing service wire formats.

## Architecture

The MCP debug path is dynamic and per debug session:

```text
MCP client
-> lmux-mcp
-> host GDB in MI mode
-> private local Unix socket under $RUNTIME/debug/
-> binary RSP bridge inside lmux-mcp
-> existing HC2 structured EXEC stdin/stdout
-> target gdbserver stdio
-> inferior
```

`lmuxd`, the target worker, and unix-rendezvous services remain binary-clean and
do not parse GDB/MI/RSP.

The older configured service:

```text
gdb:/run/lmux/gdb.sock:/tmp/lmux-gdb.sock
```

is still a manual/legacy GDB rendezvous escape hatch. P11 does not repurpose it
and does not depend on it.

## Configuration

`lmux-mcp` accepts:

```text
--gdb PATH
--target-gdbserver PATH
--max-debug-sessions N
--gdb-sysroot PATH
--gdb-solib-search-path PATH
--gdb-remote-timeout SECONDS
```

If `--gdb` is omitted, `lmux-mcp` tries `arm-linux-gnueabi-gdb` from `PATH`.
`--target-gdbserver` has no guessed default; debug start returns
`NOT_CONFIGURED` until the operator provides it.

The selected host GDB must understand the target architecture. Symbols are
loaded from the host path supplied to `debug_start`; they are not copied to the
target.

`--gdb-remote-timeout` defaults to 10 seconds. `lmux-mcp` configures host GDB
with `set remotetimeout N` before connecting to the remote target so normal
startup negotiation has more time than GDB's short default.

Example MCP configuration:

```toml
[mcp_servers.hantek]
command = ".../lmux-mcp"
args = [
  "--runtime-dir", "/run/user/1000/lmux/mcp",
  "--gdb", "/path/to/arm-linux-gnueabi-gdb",
  "--target-gdbserver", "/root/lmux/gdbserver",
  "--max-debug-sessions", "2"
]
```

## Capacity

Each debug session owns one target `gdbserver stdio` structured EXEC. It consumes
one real target EXEC slot. Command executions and debug sessions share the same
target capacity:

```text
commands + debug sessions <= 4
```

The default debug-session limit is 2 and is clamped to the target EXEC capacity.
Excess admission fails deterministically with `RESOURCE_LIMIT`.

## Tools

The MCP tools are:

```text
debug_capabilities
debug_start
debug_list
debug_status
debug_breakpoint_set
debug_breakpoint_delete
debug_continue
debug_interrupt
debug_step
debug_wait
debug_threads
debug_select_thread
debug_backtrace
debug_evaluate
debug_registers
debug_memory_read
debug_detach
debug_stop
```

`debug_start` supports:

```text
mode = attach, pid = TARGET_PID
mode = launch, program = TARGET_PATH, args = [...]
symbols = HOST_PATH
```

`debug_evaluate` uses normal GDB expression semantics and may not be
side-effect-free.

`debug_interrupt` asks host GDB to interrupt the inferior over the normal remote
protocol path. A successful tool return means the interrupt request was accepted
and sent by host GDB; callers must still observe the later stopped state through
`debug_wait` or `debug_status`. `debug_capabilities` and `debug_status` expose
this as:

```text
interrupt_transport = "gdb-rsp"
interrupt_stop_requires_wait = true
```

In all-stop remote mode, GDB sends interrupt as a raw Ctrl-C byte (`0x03`), not
as an RSP packet. Hardware tests with the Hantek vendor `gdbserver` 7.11.1 over
stdio showed that LMUX delivered this byte through the host RSP bridge, HC2, and
target EXEC stdin, but that target gdbserver did not reliably produce a stop
reply. This is treated as target debugger behavior/compatibility rather than an
LMUX transport failure. Historical gdbserver versions have had Ctrl-C/SIGIO
handling fixes, so replacing or rebuilding the old vendor gdbserver is the
preferred follow-up.

`debug_memory_read` is read-only and bounded to 64 KiB per call. P11 does not
implement memory writes, register writes, arbitrary CLI commands, multi-inferior
GDB sessions, DAP, TUI, TCP forwarding, SSH, `socat`, target debugger logic, or
RSP parsing in `lmuxd`.

## Detach And Stop

`debug_detach` is graceful: detach the inferior through GDB, shut down host GDB,
release the target gdbserver EXEC, remove the private socket, and free capacity.
Detach is intended to preserve normal GDB detach semantics for the inferior.

`debug_stop` is emergency cleanup. It does not promise detach semantics for the
inferior. It tears down broken plumbing, terminates host GDB, sends TERM and then
KILL to the target gdbserver EXEC through HC2, releases the ref, removes the
socket, and frees capacity.

## Reconnect And Worker Replacement

P11 does not promise transparent debugger survival across LMUX reconnect. If the
RSP byte stream cannot be proven continuous, the session becomes `LOST` and is
cleaned up explicitly. Worker replacement loses the target gdbserver execution
and therefore the debug session.

## Self-Debug Restriction

Do not debug the target LMUX supervisor or worker through the LMUX transport
they are carrying. Stopping those processes can deadlock the debug connection
because RSP bytes travel through that same stack. Current public host APIs do not
reliably identify those target PIDs, so P11 documents the restriction rather
than pretending to enforce it through unreliable PID guessing.
