# OUTER/1 Physical Wire Protocol

OUTER/1 is the planned stable physical framing layer between the host daemon
and the target supervisor. It carries two independent classes of traffic:

```text
TRANSPORT: opaque LMUX/2 byte-stream chunks
BOOT:      supervisor management and worker update messages
```

The target supervisor understands OUTER/1, BOOT, and opaque byte forwarding. It
must not parse LMUX/2 HELLO, streams, services, flow control, PTY, LXF, GDB, or
any worker application protocol.

This document is the wire specification. It is not an implementation note for
the current raw-LMUX/2 physical mode.

## Deferred Work

### FILE Flow-Control Performance

FILE-specific receive-window autotuning is intentionally outside OUTER/1. Future
work should add a bounded FILE-only policy such as:

```text
--file-window auto
--file-window BYTES
--file-window-max BYTES
```

The policy must cover PUT and GET, coordinate `MAX_STREAM_DATA` and `MAX_DATA`,
respect target memory limits, expose status, and include throughput and memory
regression tests. PTY and unix-rendezvous windows must not be inflated blindly.

### FILE Abort Torture

Repeated partial-transfer and SIGINT cleanup torture for FILE streams is also
deferred. It is not part of OUTER/1.

## Encoding

Each physical record is:

```text
COBS(decoded_header || payload || crc32c) || 0x00
```

The trailing zero byte is the record delimiter and is not passed to the COBS
decoder. The decoded record contains no delimiter bytes added by OUTER/1; any
zero bytes inside the decoded header, payload, or checksum are ordinary data.

All integers are little-endian. C structs are never serialized directly.

The parser accumulates bytes until a zero delimiter. It never waits on a
peer-declared length before seeing the record delimiter.

## CRC32C

OUTER/1 uses CRC-32C / Castagnoli.

Normative parameters:

```text
name:                  CRC-32C / Castagnoli
normal polynomial:     0x1EDC6F41
reflected polynomial:  0x82F63B78
algorithm form:        reflected, LSB-first
initial value:         0xffffffff
final xor:             0xffffffff
byte processing order: wire order, first decoded byte to last decoded byte
covered bytes:         decoded header || payload
excluded bytes:        crc32c field itself, COBS encoding, delimiter
wire byte order:       little-endian u32
```

Normative check values:

```text
CRC32C("")          = 0x00000000
CRC32C("123456789") = 0xe3069283
```

## Common Frame Header

Decoded v1 prefix:

```text
u8[4] magic      = "LMO1"
u16   version    = 1
u8    type
u8    flags
u16   header_len = 32
u16   reserved   = 0
u32   payload_len
u64   conn_id
u64   frame_seq
u8[payload_len] payload
u32   crc32c
```

Exact prefix byte count:

```text
magic       4
version     2
type        1
flags       1
header_len  2
reserved    2
payload_len 4
conn_id     8
frame_seq   8
----------------
total       32
```

`request_id` is deliberately not in the common header. TRANSPORT has no request
identity, HELLO is session setup, and frame sequence numbers are transport
ordering rather than operation matching. BOOT is the only OUTER/1 traffic class
that needs request/response correlation, so `request_id` belongs inside BOOT
payloads.

## Hard Bounds

V1 hard bounds:

```text
minimum header_len:              32
v1 skippable header_len maximum: 64
hard decoded payload maximum:    4096
default TRANSPORT payload target: 1024
```

`4096` is a v1 protocol implementation and resource bound, not a promise that
every peer accepts 4096 bytes. Peers advertise effective limits in HELLO. A peer
must not allocate memory directly from peer-controlled lengths.

A v1 implementation that supports `header_len <= 64` and `payload_len <= 4096`
can bound one decoded record to:

```text
64 + 4096 + 4 = 4164 bytes
```

The encoded accumulation buffer must also be bounded. For the above decoded
limit, COBS needs at most `decoded_len + ceil(decoded_len / 254)` bytes before
the delimiter, plus one delimiter byte.

## Frame Types

```text
0   invalid/reserved
1   HELLO
2   TRANSPORT
3   BOOT_REQUEST
4   BOOT_RESPONSE
5   BOOT_EVENT
6-63     reserved for future core OUTER frame types
64-127   reserved for future standard extensions
128-223  reserved for future experimental/private extensions
224-255  reserved
```

Before HELLO establishment, only valid HELLO frames may affect state. Other
valid records are discarded as pre-established traffic.

After establishment:

- known v1 frame types require v1-valid flags and header fields;
- unknown non-critical frame types are ignored after consuming the expected
  `frame_seq`;
- unknown critical frame types fail the current OUTER session;
- malformed current-session frames fail the current OUTER session.

## Flags And Header Extensibility

V1 defines one common flag bit:

```text
0x01 CRITICAL_EXTENSION
```

Bits `0x02` through `0x80` are reserved and must be zero in v1.

V1 senders set `flags == 0` for all frames defined in this document.

Header rules:

```text
header_len < 32
    invalid

header_len == 32
    normal v1

32 < header_len <= 64 and flags == 0
    v1 receiver skips the header extension bytes and interprets only the
    first 32 bytes

32 < header_len <= 64 and CRITICAL_EXTENSION set
    v1 receiver fails the current OUTER session after establishment, or
    discards before establishment

header_len > 64
    invalid for v1
```

The extension bytes are covered by CRC32C. A v1 receiver never interprets
unknown header extension bytes.

`reserved` must be zero in v1. Nonzero `reserved` is invalid.

## Endpoint Incarnations And Sessions

Each endpoint chooses a fresh nonzero random `conn_id` for each physical-link
incarnation. Host and target supervisor choose their IDs independently.

An established OUTER session is identified by the pair:

```text
(local_conn_id, peer_conn_id)
```

There is no global session ID.

Every frame sent by an endpoint carries that endpoint's current `conn_id`.
`frame_seq` is scoped to the sender `conn_id`.

## Frame Sequence Numbers

`HELLO` for a fresh `conn_id` uses:

```text
frame_seq = 0
```

The first non-HELLO frame under that `conn_id` uses:

```text
frame_seq = 1
```

Each subsequent frame increments by exactly one. Unknown non-critical frames
still consume sequence numbers when they are valid current-session frames.

Sequence numbers do not wrap. Exhaustion of `u64` sequence space forces a fresh
endpoint incarnation and a new HELLO exchange. OUTER/1 has no retransmission
protocol.

Handling after establishment:

```text
duplicate frame_seq
    current OUTER session failure

lower/stale frame_seq
    current OUTER session failure

gap above expected frame_seq
    current OUTER session failure

non-HELLO frame with wrong conn_id
    current OUTER session failure

HELLO with same peer conn_id and frame_seq 0
    duplicate HELLO; idempotent if compatible

HELLO with different nonzero peer conn_id and frame_seq 0
    peer restart/new physical incarnation
```

The physical carrier is ordered. Sequence numbers detect omission, duplication,
and stale current-session data; they do not repair it.

## HELLO

HELLO must be the first valid frame sent for a new `conn_id`.

HELLO payload v1:

```text
u8    role
u8    reserved = 0
u16   hello_len = 24
u32   capabilities
u32   max_outer_payload
u32   max_transport_payload
u32   preferred_transport_payload
u32   reserved2 = 0
```

Roles:

```text
1 HOST
2 SUPERVISOR
```

HELLO v1 fixed fields are 24 bytes. A v1 sender uses `hello_len == 24` and
`payload_len == 24`. Future longer HELLO payloads may be accepted only if the
frame is otherwise non-critical and all v1 fixed fields are valid; v1 receivers
ignore bytes beyond the first 24.

HELLO does not include implementation/build strings. BOOT `GET_INFO` is used
for version and management information.

HELLO establishment:

```text
local HELLO sent
+
peer HELLO accepted
=
OUTER ESTABLISHED
```

No TRANSPORT or BOOT frame is accepted before OUTER establishment.

## HELLO Capabilities

Initial capability bits:

```text
bit 0  TRANSPORT
bit 1  BOOT_INFO
bit 2  BOOT_UPDATE_RESERVED
bit 3  BOOT_WORKER_CONTROL_RESERVED
bits 4-31 reserved
```

Unknown capability bits are ignored. The effective operation set is the
intersection of the peers' capabilities, constrained by their roles.

Payload limits:

```text
effective_outer_payload =
    min(local_max_outer_payload, peer_max_outer_payload, 4096)

effective_transport_payload =
    min(local_max_transport_payload,
        peer_max_transport_payload,
        effective_outer_payload)
```

`preferred_transport_payload` is a scheduling hint. It must not exceed
`max_transport_payload`; if it does, the HELLO is invalid.

## HELLO Duplicates And Peer Restart

Same peer `conn_id` with a compatible duplicate HELLO is idempotent. It does
not reset sequence state, TRANSPORT, BOOT, LMS1, or LMUX.

Different peer `conn_id` while established means peer restart or a new physical
incarnation. The old OUTER session is retired:

```text
old OUTER session retired
TRANSPORT disabled
BOOT in-flight requests failed or marked stale
target supervisor performs LMS1 LINK_DOWN barrier
new HELLO establishment required
```

Host restart while target supervisor remains alive:

```text
host chooses new conn_id
target supervisor sees HELLO with different peer conn_id
target retires old OUTER peer state
target performs LMS1 LINK_DOWN/LINK_UP around TRANSPORT availability
LMUX/2 worker creates a fresh LMUX session
```

Target supervisor restart while host remains alive:

```text
target supervisor chooses new conn_id
host sees HELLO with different peer conn_id
host retires old OUTER peer state
host waits for new establishment before sending TRANSPORT or BOOT
```

Physical carrier disconnect/reconnect:

```text
carrier loss retires current OUTER session
both endpoints choose fresh conn_id for the new physical incarnation
new HELLO convergence is required
```

Convergence does not depend on heuristic autodetection or timeout-based protocol
guessing.

## Exact-Stream Failure Rule

TRANSPORT presents either:

```text
exact ordered opaque bytes
```

or:

```text
explicit OUTER link/session failure
```

It must never silently omit a corrupted TRANSPORT chunk and continue the
reconstructed byte stream.

After OUTER is established, these errors fail the current OUTER session and
bring TRANSPORT down:

```text
CRC32C mismatch
COBS decode failure
frame sequence gap/duplicate/lower value
malformed established-session frame
invalid header_len
invalid payload_len
wrong magic
nonzero reserved
unsupported critical extension
```

Before establishment, corrupt or garbage records may be discarded while looking
for a valid HELLO.

## Resynchronization

COBS plus zero delimiter is the only record boundary mechanism. The parser must
bound encoded accumulation and discard an oversized encoded record up to the
next zero delimiter.

Behavior before establishment:

```text
empty record
    ignore

oversize encoded record
    discard until delimiter

COBS decode failure
    discard record

wrong magic/version
    discard record

bad CRC
    discard record

truncated decoded record
    discard record

garbage between delimiters
    discarded as invalid records
```

Behavior after establishment:

```text
any invalid current-session record
    fail current OUTER session, disable TRANSPORT, resynchronize only for
    the next HELLO/session
```

## TRANSPORT

TRANSPORT payload is arbitrary contiguous bytes from the LMUX/2 worker byte
stream.

Normative rule:

```text
LMUX frame boundaries have no relationship to TRANSPORT boundaries.
```

The encapsulator may split or coalesce LMUX byte ranges. The decapsulator
reconstructs exact byte order and writes bytes to the next layer.

TRANSPORT has no `request_id`, acknowledgement, retransmission, or stream ID.
If exact byte delivery cannot be preserved, OUTER fails the session and
TRANSPORT goes down.

## Target Worker Interaction

Target supervisor may issue LMS1 `LINK_UP` only after OUTER is established:

```text
physical carrier open
    |
OUTER HELLO handshake
    |
OUTER ESTABLISHED
    |
LMS1 LINK_UP barrier
    |
worker fresh LMUX HELLO
    |
TRANSPORT carries LMUX bytes
```

On OUTER failure:

```text
TRANSPORT disabled immediately
    |
LMS1 LINK_DOWN barrier
```

Namespaces are distinct:

```text
OUTER conn_id: endpoint physical-link incarnation
LMS1 epoch:    supervisor/worker local link-control barrier epoch
LMUX conn_id:  LMUX/2 worker transport connection incarnation
```

Do not compare or reuse values across these namespaces.

## BOOT Common Payloads

BOOT request identity is inside BOOT payloads.

`BOOT_REQUEST` payload:

```text
u16 opcode
u16 flags
u64 request_id
u8[] body
```

`BOOT_RESPONSE` payload:

```text
u16 opcode
u16 status
u32 reserved = 0
u64 request_id
u8[] body
```

`BOOT_EVENT` payload:

```text
u16 event
u16 flags
u32 reserved = 0
u64 event_seq
u8[] body
```

Request IDs are host-generated and scoped to the current OUTER/BOOT session.
`request_id == 0` is invalid for requests.

Initially, only one mutating BOOT request may be outstanding. Read-only
requests may also be limited to one outstanding by the implementation. Duplicate
idempotent requests with the same `request_id` may repeat the previous response.
A stale or unknown `request_id` receives a BOOT error if the OUTER session is
otherwise valid.

BOOT status values:

```text
0 OK
1 UNKNOWN_OPCODE
2 BAD_REQUEST
3 BUSY
4 UNSUPPORTED
5 NO_SPACE
6 BAD_HASH
7 NOT_FOUND
8 WRONG_STATE
9 INTERNAL_ERROR
```

Unknown BOOT opcodes receive `UNKNOWN_OPCODE`; they do not fail OUTER framing.

## BOOT Opcodes

Implemented first read-only operations:

```text
1 GET_INFO
2 WORKER_STATUS
```

Reserved future update/control operations:

```text
16 UPLOAD_BEGIN
17 UPLOAD_DATA
18 UPLOAD_END
19 CANDIDATE_STATUS
20 ACTIVATE_CANDIDATE
21 CONFIRM_CANDIDATE
22 ROLLBACK
23 RESTART_WORKER
```

Opcode ranges:

```text
0       invalid
1-15    read-only/status operations
16-63   mutating update operations
64-127  worker lifecycle/control operations
128-32767 reserved for future standard operations
32768-65535 experimental/private
```

`GET_INFO` request body is empty.

`GET_INFO` response body:

```text
u16 outer_version
u16 boot_version
u32 capabilities
u32 max_outer_payload
u32 max_transport_payload
u32 reserved = 0
```

`WORKER_STATUS` request body is empty.

`WORKER_STATUS` response body:

```text
u32 worker_state
u32 flags
u64 worker_generation
u64 uptime_ms
u32 restart_count
u32 reserved = 0
```

Worker states:

```text
0 UNKNOWN
1 STOPPED
2 STARTING
3 LINK_DOWN
4 LINK_UP_BARRIER
5 ACTIVE
6 BACKOFF
```

## BOOT Update Model

The initial update target is the worker binary, not the supervisor binary and
not the whole firmware image. Supervisor self-update is explicitly out of scope
for initial BOOT update work.

Recommended persistent layout:

```text
/root/lmux/workers/<sha256>.lmuxd
/root/lmux/worker.state
```

`worker.state` is the single authoritative state file. Human-friendly symlinks
may exist but are non-authoritative.

Conceptual state:

```text
generation
known_good
active
candidate
mode = idle | uploading | test-active | confirmed | rollback-needed
```

Supervisor chooses the executable from:

```text
workers/<active>.lmuxd
```

Durability model for `worker.state`:

```text
write worker.state.tmp
fsync(worker.state.tmp)
rename(worker.state.tmp, worker.state)
fsync(parent directory)
```

Candidate binary installation uses the same pattern:

```text
write temp candidate
fsync(temp)
rename(temp, workers/<sha256>.lmuxd)
fsync(parent directory)
```

## Upload Resume

Future upload operations use offset resume.

`UPLOAD_BEGIN(size, sha256)`:

```text
returns current expected offset for this candidate identity
```

`UPLOAD_DATA(offset, bytes)`:

```text
accepted only when offset == expected offset
```

`UPLOAD_END`:

```text
fsync candidate
compute SHA-256 over the complete stored file
verify against declared SHA-256
atomically install content-addressed worker binary
```

The supervisor does not persist an implementation-specific SHA context. On
resume, final full-file hash verification is authoritative.

SHA-256 provides integrity and identity, not authorization or authenticity. If
the first update implementation accepts unsigned worker images, then anyone
capable of speaking BOOT over the trusted physical link can install arbitrary
worker code. Signature policy is future work unless requested separately.

## Activate, Confirm, And Rollback

Candidate activation lifecycle:

```text
known-good worker A
    |
upload and verify candidate B
    |
ACTIVATE_CANDIDATE
    |
mode = test-active, active = B
    |
supervisor starts B
    |
health probation
    |
CONFIRM_CANDIDATE
    |
B becomes known_good
```

First health definition:

```text
exec succeeds
LMS1 sideband is compatible
LINK_UP_ACK succeeds while the physical link is available
worker survives probation interval
```

No LMS1 `WORKER_READY` message is added initially.

If a supervisor/process/device restart occurs while `worker.state` says
`test-active`, the conservative v1 behavior is rollback to `known_good`.
Physical carrier disconnect by itself is not candidate failure. A candidate
remains unconfirmed until explicit `CONFIRM_CANDIDATE`.

## State Machines

Host OUTER states:

```text
DOWN
    carrier absent/closed

WAIT_HELLO
    carrier open, local HELLO sent or ready, peer HELLO not accepted

ESTABLISHED
    local HELLO sent and peer HELLO accepted

ERROR_RESETTING
    current session failed; TRANSPORT disabled; prepare fresh incarnation
```

Target supervisor OUTER states:

```text
DOWN
    physical carrier absent/closed

WAIT_HELLO
    physical carrier open, OUTER HELLO convergence in progress

ESTABLISHED
    OUTER session established; TRANSPORT may be enabled after LMS1 barrier

ERROR_RESETTING
    current OUTER session failed; TRANSPORT disabled; LMS1 LINK_DOWN required
```

Target supervisor relationship to LMS1 and LMUX/2:

```text
OUTER DOWN
    LMS1 LINK_DOWN stable
    LMUX/2 session absent/retired

OUTER WAIT_HELLO
    LMS1 LINK_DOWN stable
    LMUX/2 session absent/retired

OUTER ESTABLISHED, LMS1 LINK_UP barrier pending
    worker has not yet exposed current TRANSPORT as active LMUX carrier

OUTER ESTABLISHED, LMS1 active
    TRANSPORT carries opaque LMUX/2 bytes
```

## Golden Wire Examples

The examples below include the decoded record with CRC and the final
COBS-encoded record including the trailing delimiter.

Example constants:

```text
host conn_id       = 0x1122334455667788
supervisor conn_id = 0x99aabbccddeeff00
capabilities       = TRANSPORT | BOOT_INFO = 0x00000003
max_outer_payload  = 4096
max_transport      = 1024
preferred_transport = 1024
```

### HELLO

Host HELLO, `frame_seq = 0`, role `HOST`.

```text
CRC32C = 0xe032aa05
decoded =
4c4d4f310100010020000000180000008877665544332211000000000000000001001800030000000010000000040000000400000000000005aa32e0

encoded =
064c4d4f310102010220010102180101098877665544332211010101010101010201021802030101010210010102040101020401010101010505aa32e000
```

### TRANSPORT

TRANSPORT from host, `frame_seq = 1`, payload:

```text
00 03 41 00 ff
```

```text
CRC32C = 0xe4d9ef8c
decoded =
4c4d4f310100020020000000050000008877665544332211010000000000000000034100ff8cefd9e4

encoded =
064c4d4f3101020202200101020501010a8877665544332211010101010101010103034106ff8cefd9e400
```

### BOOT GET_INFO Request

BOOT_REQUEST from host, `frame_seq = 2`, opcode `GET_INFO`, `request_id =
0x0102030405060708`.

```text
CRC32C = 0xab2927a6
decoded =
4c4d4f3101000300200000000c00000088776655443322110200000000000000010000000807060504030201a62729ab

encoded =
064c4d4f3101020302200101020c01010a887766554433221102010101010101020101010d0807060504030201a62729ab00
```

### BOOT GET_INFO Response

BOOT_RESPONSE from supervisor, `frame_seq = 7`, opcode `GET_INFO`, status `OK`,
`request_id = 0x0102030405060708`.

```text
CRC32C = 0x90a12fa8
decoded =
4c4d4f3101000400200000002400000000ffeeddccbbaa990700000000000000010000000000000008070605040302010100010003000000001000000004000000000000a82fa190

encoded =
064c4d4f3101020402200101022401010109ffeeddccbbaa990701010101010102010101010101010a08070605040302010102010203010101021001010204010101010105a82fa19000
```

## Source Ownership Plan

Planned implementation ownership:

```text
src/link/
    link_protocol.c
    link_protocol.h
    link_endpoint.c
    link_endpoint.h

src/host/
    host_link.c

src/supervisor/
    supervisor_link.c
```

`src/link/` owns generic OUTER codec, session, bounded queues, and scheduling
mechanics. Host-specific integration stays under `src/host/`. Target
supervisor integration with LMS1 and worker lifecycle stays under
`src/supervisor/`.

OUTER is not a carrier backend and not LMUX/2 transport. It sits between the
byte-stream carrier and the opaque LMUX/2 worker byte stream.

