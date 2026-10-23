# LMUX Wire 1.0.1 Release Notes

LMUX Wire 1.0.1 is a patch release for the host HC2/MCP FILE lifecycle and
durable PTY re-attach presentation. It does not change LMUX/2, OUTER/1, LXF1,
carrier framing, target FILE semantics, or the published `v1.0.0` release.

## FILE lifecycle fixes

- Long-running MCP FILE operations are owned by host `lmuxd`, not by the
  lifetime of one MCP or HC2 request.
- Asynchronous `file_put_start` / `file_get_start`, status, bounded wait, and
  explicit cancellation expose `RUNNING -> DONE | FAILED | CANCELLED | LOST`.
- START uses a caller-supplied request token in a private MCP session namespace.
  Repeating a retained token with identical parameters returns the same
  operation; conflicting parameters are rejected.
- Status by request token recovers both active and retained terminal operations
  when the original START response was lost, including after HC2 reconnect.
- The race in which FILE completion arrived before MCP installed its local
  waiter is removed by authoritative daemon status.
- HC2 preserves the original `errno` across FILE start cleanup, so normalized
  error code and diagnostic text cannot refer to different failures.
- `file_operation_wait` defaults to 15 seconds, is capped at 20 seconds, keeps a
  response reserve before an exposed MCP deadline, and does not busy-poll.
- Cancellation under FILE backpressure closes the stream, records a terminal
  result, removes temporary local GET output, and releases the active slot.
- HC2 disconnect does not abort daemon-owned FILE work. LMUX/worker replacement
  records `LOST`; a new incarnation never inherits a stale active operation.
- Terminal FILE notifications use unsolicited HC2 frames, avoiding request-ID
  reuse collisions after reconnect.
- MCP exposes FILE operation IDs as decimal strings. The deprecated
  `file_op_id` alias in synchronous PUT/GET results is also a string, preventing
  JSON/JavaScript consumers from rounding randomly seeded IDs above `2^53-1`.

## Retention and ambiguous outcomes

`lmuxd` retains eight FILE operation records in memory. Request-token
idempotency is guaranteed only while the record remains in that bounded table
and during the current daemon lifetime. After eviction or daemon restart,
status reports an unknown/ambiguous outcome. This does not prove that START was
not executed and callers must not automatically retry PUT or GET.

The namespace is tied to the current `lmux-mcp` session. It survives replacement
of the HC2 connection by that process, but not restart/replacement of the MCP
process itself. No durable database or unbounded registry is introduced.

## Compatibility

The HC2 handshake advertises a new `FILE_LIFECYCLE` capability. A 1.0.1 MCP
client fails with an explicit compatibility diagnostic when connected to a
1.0.0 daemon that lacks the lifecycle/idempotency operations. A 1.0.0 HC2 client
can continue using the legacy FILE START frames against a 1.0.1 daemon.

The target FILE engine, LXF temporary-file and atomic-rename behavior, SHA-256,
`fsync`, bounded flow control, structured EXEC, PTY lifetime/single-attachment,
GDB, generic streams, reconciliation, and deploy contracts are unchanged. This
release makes no new power-loss durability claim because parent-directory
`fsync` is still outside the FILE contract.

## Durable PTY attach reset

Detached PTY output is drained and discarded; completed commands and old
scrollback are never replayed. The earlier bounded current-line/ANSI
reconstruction has been removed. The fixed 16 KiB buffer now serves only as a
pending live-output queue while an attachment exists.

For the standard target BusyBox ash, a new attachment recognizes its exact
idle line-editor tty mode and uses `TIOCSIG(SIGINT)` on the PTY master. This
resets ash's real unfinished edit buffer and makes ash print its current prompt,
so a later Enter cannot execute hidden pre-detach input. No byte is injected
into PTY input. A shell's normal interrupt marker may be visible before the
prompt.

Because the PTY signal is asynchronous, input from the new attachment remains
gated until reset output is observed. Old detached output is discarded before
the signal. Failure to observe reset output within a bounded deadline closes
that attachment instead of allowing a newly typed command to be split across
the interrupt.

The reset is skipped unless the durable shell is itself the foreground process
group and the tty flags exactly match the saved baseline with only
`ICANON/ECHO/ISIG` disabled. A running external command, builtin in normal tty
mode, raw-mode application, or unrecognized shell therefore receives neither a
signal nor synthetic input. LMUX does not provide generic readline or screen
restoration in those cases.

## MCP GDB memory presentation

`debug_memory_read` retains its compatible base64 response by default and now
accepts `format=hex` or `format=both`. The hex form is generated directly in
the host `lmux-mcp` process as a bounded 16-byte-per-line address/hex/ASCII
dump. Line addresses use the actual start address returned by GDB/MI. No GDB,
HC2, LMUX/2, OUTER/1, RSP bridge, or target behavior changes.

## Verification status

Software verification covers lost START responses in RUNNING and DONE states,
duplicate/conflicting tokens, HC2 reconnect, bounded-history eviction, MCP wait
deadlines, cancellation under stopped-worker backpressure, worker replacement,
large transfer, subsequent FILE reuse, and concurrent structured EXEC.
PTY process integration additionally covers the first BusyBox prompt, real
partial-input reset, suppression of old transcript, live-output ordering,
bounded detached output, foreground command/builtin and raw-mode safety, and
repeated detach/re-attach while preserving the shell process.

Hardware acceptance has not yet been performed. LMUX Wire 1.0.1 must not be
described as hardware-verified until the separate hardware gate is completed.
