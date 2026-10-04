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
command_start
command_status
command_read
command_write_stdin
command_close_stdin
command_signal
command_release
```

`command_start` is argv-based. It does not imply a shell. Use:

```json
{"argv":["/bin/sh","-c","printf hi"]}
```

when shell behavior is wanted.

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
