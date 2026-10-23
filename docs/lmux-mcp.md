# lmux-mcp

`lmux-mcp` is the standalone host-side MCP frontend for LMUX Wire.

It runs as a separate host process:

```text
MCP client
  -> lmux-mcp
  -> /run/lmux/control-v2.sock
  -> host lmuxd
```

It does not start the legacy `lmux` CLI, does not add a public `lmux exec`
command, and does not run code on the target beyond the existing LMUX services.

## Build

The host build creates:

```text
build/host/lmux-mcp
```

The target build remains C-only and does not include Go.

## Run

Default:

```sh
build/host/lmux-mcp
```

Useful development overrides:

```sh
build/host/lmux-mcp \
  --control /run/lmux/control-v2.sock \
  --runtime-dir "$XDG_RUNTIME_DIR/lmux/mcp"
```

MCP protocol messages use the official Go SDK stdio transport. `lmux-mcp` is
not an interactive CLI; running it in a terminal waits for MCP JSON-RPC messages
on stdin. Do not write human logs to stdout. Human observation uses the session
journal.

Codex configuration is conceptually:

```toml
[mcp_servers.lmux]
command = "/absolute/path/to/build/host/lmux-mcp"
args = ["--runtime-dir", "/run/user/1000/lmux/mcp"]
```

When Codex supplies `XDG_RUNTIME_DIR`, the `--runtime-dir` argument can be
omitted.

The default structured-control endpoint remains:

```text
/run/lmux/control-v2.sock
```

This socket is owned by host `lmuxd`, not by `lmux-mcp`. `lmux-mcp` only needs
permission to connect to it.

## Runtime Files

Default layout:

```text
$XDG_RUNTIME_DIR/lmux/mcp/
  owner.lock
  current -> sessions/session-<id>
  sessions/
    session-<id>/
      session.log
      session.log.1
      command-<token>.transcript
```

`session.log` is JSON Lines and can be watched with:

```sh
tail -F "$XDG_RUNTIME_DIR/lmux/mcp/current/session.log"
```

The journal is not a control interface and is not the source of MCP cursors.

If `XDG_RUNTIME_DIR` is not set, pass `--runtime-dir PATH` or set
`LMUX_MCP_RUNTIME_DIR`. `lmux-mcp` does not fall back to `/run/lmux/mcp`, because
that path is system-owned on normal hosts and would require sudo.

## Tools

Implemented tools:

```text
lmux_status
file_put
file_get
file_put_start
file_get_start
file_operation_status
file_operation_wait
file_operation_cancel
command_start
command_status
command_read
command_write_stdin
command_close_stdin
command_signal
command_release
debug_capabilities
debug_start
debug_list
debug_status
debug_diagnostics_read
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

`debug_memory_read` accepts an optional `format`:

- omitted or `base64`: the compatible `{base64, bytes}` response;
- `hex`: `{address, hex, bytes}` with a 16-byte-per-line address/hex/ASCII dump;
- `both`: `{address, hex, base64, bytes}`.

The `address` in formatted responses is the actual starting address resolved by
GDB, not merely the caller's expression. Reads remain bounded to 64 KiB, which
also bounds the expanded `hex` and `both` responses. Formatting is performed
inside `lmux-mcp`; no shell or external dump utility is used.

`file_put` and `file_get` keep their convenient wait-for-completion behavior
for short transfers. They accept an optional caller-generated `request_token`;
when omitted, MCP creates an internal token for compatibility. For a transfer
that may outlive an MCP tool deadline, choose a unique token before the call and
pass it to `file_put_start` or `file_get_start`:

```json
{"local_source":"/host/image.bin","target_destination":"/tmp/image.bin","request_token":"agent-job-42-image"}
```

The asynchronous START tools require this token. Retain the token even if the
whole tool response is lost. Recover with
`file_operation_status(request_token=...)`, which finds both RUNNING and
retained terminal operations, then use the returned opaque decimal
`operation_id` for WAIT or CANCEL. Repeating START with the same token and
identical parameters returns the same operation; different parameters are an
explicit conflict. `file_operation_cancel` returns `CANCELLED` only after
the host daemon has closed and released the active FILE stream. If the cancel
request itself loses its HC2 connection, its result is deliberately reported as
unknown; query status after reconnect instead of repeating the transfer.

`file_operation_wait` defaults to 15 seconds, is capped at 20 seconds, and
shortens itself when the MCP request context exposes an earlier deadline. It
reserves 1.5 seconds for the final HC2 status round trip and MCP response. A
local wait expiry returns `RUNNING` with `wait_timed_out=true`; MCP cancellation
remains a cancellation and never cancels the daemon-owned FILE operation.

FILE operation IDs are decimal strings in MCP JSON. The synchronous `file_put`
and `file_get` results retain the deprecated `file_op_id` compatibility field,
but it too is a string and is identical to the canonical `operation_id`.
Callers must not coerce either field to a JavaScript `Number`; pass the string
unchanged to STATUS, WAIT, or CANCEL.

Idempotency records share the bounded eight-operation daemon history. After
eviction or daemon restart, token lookup returns `UNKNOWN` with
`outcome_ambiguous=true`. This is not evidence that START did not execute and
must not trigger an automatic retry. The namespace is the random current MCP
session ID, so recovery survives HC2 reconnect but not `lmux-mcp` process/session
replacement.

`debug_interrupt` returns when host GDB accepts/sends the interrupt request over
the remote protocol. It does not by itself prove the inferior stopped; use
`debug_wait` or `debug_status` to observe the later `STOPPED` state. Capabilities
report `interrupt_transport="gdb-rsp"` and
`interrupt_stop_requires_wait=true`.

`command_start` is argv-based. It does not imply a shell. Use:

```json
{"argv":["/bin/sh","-c","printf hi"]}
```

when shell behavior is wanted.

Debug tools are documented in [debugging.md](debugging.md). They use a host GDB
process plus a target `gdbserver stdio` structured EXEC and are separate from
normal command sessions, while sharing the same target EXEC capacity.

## Retention

V1 defaults:

```text
max retained sessions: 5
max storage/session: 10 MiB
journal: session.log + session.log.1, about 1 MiB total
```

Transcript overflow drops oldest retained transcript records and advances the
cursor floor. Old cursors return `CURSOR_EXPIRED`; they are not silently moved.

## Worker Loss

Command tokens are separate from target `exec_id` and structured `exec_ref`.
If the target worker is replaced, affected commands become `LOST`. Already
spooled transcript remains readable until release/session GC, but stdin,
signals, and target status/control fail for that command.
