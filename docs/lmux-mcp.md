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
  --runtime /run/lmux/mcp
```

MCP protocol messages use stdio. Do not write human logs to stdout. Human
observation uses the session journal.

## Runtime Files

Default layout:

```text
/run/lmux/mcp/
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
tail -F /run/lmux/mcp/current/session.log
```

The journal is not a control interface and is not the source of MCP cursors.

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
