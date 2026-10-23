# LMUX/2 Wire Protocol

LMUX/2 is the replacement transport for LMUX Wire. It is not negotiated with
LMUX/1 and there is no fallback. A peer that does not produce valid LMUX/2
frames is rejected at connection scope.

## Encoding

Each frame is:

```text
COBS(decoded_frame || crc16) || 0x00
```

The trailing zero byte is the record delimiter and is not included in the COBS
decoder input. The CRC is CRC-16/CCITT-FALSE over the decoded header and
payload, before the CRC field itself.

All integer fields are little-endian. C structs are never serialized directly.

## Frame Header

```text
u8[4] magic = "LMX2"
u8    version = 2
u8    type
u16   flags
u64   stream_id
u64   value
u16   payload_len
u16   reserved = 0
u8[payload_len] payload
u16   crc16
```

The current maximum payload is 512 bytes. Larger decoded payload lengths are
rejected by the codec.

## Types

```text
1 HELLO
2 STREAM_OPEN
3 STREAM_OPEN_OK
4 STREAM_OPEN_ERROR
5 STREAM_CLOSE
6 STREAM_RESET
7 DATA
8 MAX_STREAM_DATA
9 MAX_DATA
10 CONTROL
```

Unknown non-critical frame types are ignored. Unknown critical frame types use
flag bit `0x0001` and are connection protocol errors. Known frame types
currently require `flags == 0`.

## HELLO

`HELLO` is sent on stream 0. Payload:

```text
u8    role: 1 host, 2 target
u8[3] reserved = 0
u64   connection_id
u32   max_streams
u32   stream_window
u64   connection_window
```

HELLO establishes one physical LMUX/2 connection and one stream-id namespace.
Duplicate HELLO for the same peer connection is harmless. A new physical
connection starts a new stream-id namespace.

## Roles And Authority

The host is authoritative for topology and service attachment decisions.
That is control-plane authority, not asymmetric transport. Once a stream exists,
both sides are transport peers. Either side may send `STREAM_CLOSE`,
`STREAM_RESET`, flow-control updates, and connection-level errors when local
state requires it.

## Lifetime Identifiers

```text
service name: durable logical identity inside one worker lifetime
service_id:   runtime handle inside one worker lifetime
stream_id:    transient identity inside one physical LMUX/2 connection
```

After physical reconnect without worker restart, `node_id`, `boot_id`, and
service objects may survive, but all old streams are terminal and the stream-id
namespace starts fresh. After worker restart, `boot_id` changes and the host
must discard cached service IDs, reconciling by service name.

The current daemon keeps service IDs process-local to the worker. They are not
durable across worker restart and must be rediscovered after a new `boot_id`
scheme is added.

## Stream IDs

Stream 0 is reserved for connection/control. Host-created streams are odd.
Target-created streams are even. IDs increase monotonically by 2 and are never
reused within one physical LMUX/2 connection.

Streams are created only by `STREAM_OPEN` followed by `STREAM_OPEN_OK`.
`DATA` never creates a stream.

## Stream Lifecycle

Minimum states implemented by the core:

```text
UNUSED
OPEN_SENT
OPEN_RECEIVED
OPEN
LOCAL_CLOSED
REMOTE_CLOSED
RESET
TERMINAL
```

`STREAM_OPEN` carries the stream ID and `service_id` in `value`. The payload is
either empty or an 8-byte little-endian pending-client token for services that
reported a target-side pending endpoint.
`STREAM_OPEN_OK` accepts it. `STREAM_OPEN_ERROR` rejects it. `STREAM_CLOSE` is a
graceful half-close; after both sides close the stream is terminal.
`STREAM_RESET` terminates the stream immediately.

Application/stream errors are stream-scoped whenever possible and must not
reset the physical connection or unrelated streams.

## DATA

`DATA` carries a byte payload and uses `value` as the absolute stream offset.
The receiver accepts only:

```text
offset == expected_recv_offset
```

On success, `expected_recv_offset += payload_len`. LMUX/2 does not implement
retransmission or reordering. Duplicate or out-of-order DATA on a known active
stream is stream-scoped protocol error and resets that stream. DATA for an
unknown or terminal old stream is ignored as stale and cannot revive a stream.

## Flow Control

LMUX/2 uses cumulative absolute limits:

```text
MAX_STREAM_DATA(stream_id, absolute_limit)
MAX_DATA(0, absolute_connection_limit)
```

Sender invariants:

```text
stream send_offset <= peer_max_stream_data
connection bytes_sent <= peer_max_data
```

Duplicate or lower limit updates are idempotent and harmless. Limit arithmetic
uses 64-bit counters and checks overflow. Receiver increases advertised limits
only when local buffered data is consumed.

## Resource Bounds

The current transport core has fixed bounds:

```text
max active streams: 16
per-stream RX ring: 1024 bytes
per-stream TX ring: 1024 bytes
connection RX window: 4096 bytes
outgoing lifecycle/control queue: 32 frames
frame payload: 512 bytes
```

No allocation is made based on a peer-controlled payload length.

## Error Scope

Connection-scoped errors include invalid frame encoding, bad CRC, bad protocol
magic/version, bad HELLO, invalid peer role, invalid peer stream-id parity,
known frame flags, and connection-wide flow-control violation.

Stream-scoped errors include bad stream state transitions, DATA before a stream
is open, stream offset mismatch, per-stream flow-control violation, and local
application endpoint failure.

## Stream 0 Control Plane

`CONTROL` frames always use `stream_id == 0`. They are not JSON and do not
serialize C structs. Payload:

```text
u8    control_version = 1
u8    kind
u16   reserved = 0
u32   request_id
u64   service_id
u64   token
TLV[] typed payload
```

Each TLV is:

```text
u16 type
u16 length
u8[length] value
```

All strings are bounded NUL-terminated byte strings. A receiver validates every
length before copying. Unknown control kinds and TLVs are ignored unless the
surrounding operation requires a known field.

Current control kinds:

```text
1 FACTORY_REPORT
2 SERVICE_ENSURE
3 SERVICE_OK
4 SERVICE_ERROR
5 SERVICE_DELETE
6 SERVICE_STATE
7 SERVICE_EVENT
8 SERVICE_CONTROL
```

Current TLVs:

```text
1 NAME
2 SERVICE_TYPE
3 TARGET_PATH
4 ERROR_CODE
5 EVENT
6 ARGV
7 SUPERVISED
8 WINSIZE
```

Current service type:

```text
1 unix-rendezvous
2 pty
3 file
```

`FACTORY_REPORT` is target-to-host and currently reports `unix-rendezvous`,
`pty`, and `file`.
`SERVICE_ENSURE` is host-to-target. For `unix-rendezvous`, it carries service
name, service type, and target UNIX socket path. If the name does not exist, the
target creates the service. If it exists with an identical definition, the
target returns `SERVICE_OK` for the existing object. If it exists with a
different definition, the target returns `SERVICE_ERROR CONFLICT` and leaves the
old service alive.

For `pty`, `SERVICE_ENSURE` carries service name, service type, `ARGV`, and
`SUPERVISED`. The current implementation supports a single executable path such
as `/bin/sh` and supervised=true. A PTY service owns the PTY master and child
process for the service lifetime. Stream detach, host client disconnect, and
physical LMUX reconnect do not kill the child. `SERVICE_DELETE` or daemon
shutdown terminates it.

`SERVICE_DELETE` removes the target service, closes pending endpoints and active
bindings, closes the listener, and unlinks only the listener path created by
that daemon process. It does not imply connection reset.

`SERVICE_EVENT PENDING_CLIENT` is target-to-host and carries a bounded token in
the control header. The target does not open a host-visible stream by itself.
The host may later attach by sending `STREAM_OPEN(service_id, token)`. Tokens
are invalidated by abandon, pairing, service deletion, or connection loss. A
stale token receives `STREAM_OPEN_ERROR` and must not attach to another client.

PTY services do not use pending target-side clients. Host attachment is
`STREAM_OPEN(service_id)` with an empty payload. The target allows one active
attachment initially; a second simultaneous attach receives `STREAM_OPEN_ERROR`
without creating another child process.

For `file`, `SERVICE_ENSURE` carries only service name and service type in the
current implementation. The file factory has no target-side path/configuration.
The service is durable, but every local `file.sock` client is a transient LMUX/2
stream carrying the existing LXF1 application protocol as opaque bytes. LMUX/2
does not parse PUT, GET, paths, hashes, or filesystem semantics in the transport
core. The target adapter allows one active file stream/session at a time; a
second simultaneous attach receives `STREAM_OPEN_ERROR`.

LXF1 remains the application protocol above the stream. Its record boundaries
are independent of LMUX/2 DATA frame boundaries and may span many DATA frames.
PUT keeps the existing safety model: write to a temporary file, stream bounded
DATA records, verify size/SHA256 on DONE, fsync/close using the existing policy,
then atomic rename on success. Bad hash or disconnect aborts the session and
removes the temporary file without replacing the destination. GET emits the
existing OK/DATA/DONE records with size and SHA256 behavior unchanged. Physical
LMUX reconnect aborts any active file stream/session; the durable `file` service
remains coherent and the host opens a fresh stream after reconciliation.

This is an atomic replacement contract across process or transport interruption.
It is not a power-loss durability promise for the destination parent directory;
the FILE service does not currently fsync the parent directory after rename.

`LXF_T_CWD_REQUEST` is still an LXF application message, not LMUX/2 transport.
For compatibility, the current target adapter maps CLI console context 1/2 to
PTY services `console1`/`console2` and reports `/proc/<shell-pid>/cwd` when
available.

`SERVICE_CONTROL` is a service-specific control message. For PTY services,
`TLV_WINSIZE` contains:

```text
u16 rows
u16 cols
u16 xpixel
u16 ypixel
```

The target applies it to only that PTY service with `TIOCSWINSZ`. This is not a
generic transport frame type. The host local control socket accepts
`WINSIZE service rows cols [xp yp]`; for compatibility with the current CLI,
`1` and `2` are aliases for `console1` and `console2`.

The target does not retain scrollback or reconstruct a terminal line. While no
client is attached it drains and discards PTY output, preserving poll-loop
fairness without accumulating a transcript. The fixed 16 KiB PTY buffer is
only a pending-output queue for an active attachment; normal stream flow
control protects every queued live byte.

On a new attachment the target compares the current tty flags with the saved
PTY baseline and checks that the foreground process group is the durable shell
itself. The automatic input reset is attempted only for the exact BusyBox ash
idle-editor transformation: unchanged input/output/control flags and local
flags equal to the baseline with `ICANON`, `ECHO`, and `ISIG` disabled. It uses
Linux `TIOCSIG(SIGINT)` on the PTY master, so no synthetic byte is placed in the
input stream. Ash clears its own edit buffer and emits its current prompt.
Before signaling, the worker discards any output that was still pending from
the detached interval. Input received on the new stream is held until the PTY
produces reset output, so an immediately typed command cannot be split by the
asynchronous signal. If that causal confirmation does not arrive within the
bounded reset deadline, the attachment is closed rather than risking delivery
into an ambiguous old edit state.

If the foreground group differs, or any tty flag is not recognized, attachment
has no side effect on the PTY process. This protects external commands,
builtins in the baseline tty mode, and raw-mode applications. Such an
attachment receives only subsequent live bytes; LMUX neither invents a prompt
nor claims generic readline/fullscreen restoration. The check and signal run
once when the target accepts that stream, and the reset gate is owned by that
stream, so transport reconciliation cannot repeat it for one attachment.

## Runtime Bounds

The first daemon-integrated LMUX/2 slice adds these fixed runtime bounds:

```text
services per daemon: 8
desired host services: 8
active endpoint bindings per service: 8
PTY pending attachment output per service: 16384 bytes
service name bytes including NUL: 32
UNIX socket path bytes including NUL: 108
poll slots per loop: 64
```

The runtime service adapters currently implemented are `unix-rendezvous`, `pty`,
and `file`. Rich public `control.sock` status and host topology configuration
files are intentionally later phases.
