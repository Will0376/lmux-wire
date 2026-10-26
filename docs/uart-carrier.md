# Named Carrier Architecture

UART is a physical carrier for the existing OUTER/1 byte stream. It does not
add an LMUX/2 transport or change OUTER/1, LXF1, or service wire protocols.
Carrier identity is deliberately independent of device naming and physical
technology.

> **Deployment warning:** a shared-console DIRTY UART is not a drop-in RAW
> carrier and is not enabled merely by naming a serial device. It requires a
> prepared sole-RX dispatcher, target supervisor IPC, matching host endpoint,
> and a tested console/recovery procedure. A dedicated binary-clean UART can
> use RAW without the console dispatcher. See
> [Shared-console DIRTY deployment](uart-dispatcher.md#shared-console-dirty-deployment)
> for the normative ownership, startup, failure, and readiness guidance.

## Identity model

Every configured endpoint has four separate properties:

- `name`: stable logical identity, such as `primary` or `rescue`;
- `type`: I/O backend, currently `tty` in production;
- `mode`: `raw` or `dirty` carrier framing;
- `state`: `down`, `listening`, `open`, `failed`, or administratively
  `released`, plus a separate active role owned by the manager.

Names contain 1 through 31 characters. The first character is alphanumeric;
the remainder may also contain `.`, `_`, and `-`. Matching is exact and
case-sensitive. Empty, invalid, overlong, and duplicate names are rejected.

The manager is a fixed two-entry table with no heap allocation for endpoint
identity. Local table indices are implementation details and must never appear
in LMC1. Exactly one configured endpoint is authoritative; exactly one OUTER/1
instance uses it. An endpoint may be down without preventing the other endpoint
from opening or remaining active.

With the current structures, `sizeof(struct lmux_carrier_endpoint)` is 1032
bytes and `sizeof(struct lmux_carrier_manager)` is 2080 bytes on the ARM EABI5
target (1112 and 2240 bytes respectively on the x86_64 host). These fixed
bounds include the later bounded dispatcher/diagnostic carrier state and the
administrative flag; names, endpoint table entries, and reopen state allocate
no heap memory.

The target supervisor owns its carrier manager. The worker continues to own
LMUX/2 and services and sees carrier changes only through the existing LMS1
LINK_DOWN/LINK_UP barrier. A future TCP backend may have a different open and
accept lifecycle, so the manager depends on carrier operations rather than on
TTY-specific identity. B1.1 does not implement TCP or a plugin registry.

## Startup configuration

The unchanged legacy target command remains valid:

```text
lmuxd --supervisor --outer --carrier /dev/ttyGS0
```

`--carrier PATH` creates the active TTY endpoint named `primary`. The name can
be overridden with `--carrier-name NAME`. One named standby TTY can be added:

```text
lmuxd --supervisor --outer \
  --carrier /dev/ttyGS0 \
  --standby-carrier rescue=/dev/ttyS0 \
  --standby-baud 115200 \
  --standby-rtscts
```

Host and target names must agree, while paths are explicitly local:

```text
host:   primary=/dev/ttyACM0  rescue=/dev/ttyUSB0
target: primary=/dev/ttyGS0   rescue=/dev/ttyS0
```

The startup scripts use `LMUX_CARRIER`, `LMUX_CARRIER_NAME`,
`LMUX_STANDBY_CARRIER=NAME=PATH`, `LMUX_STANDBY_BAUD`, and
`LMUX_STANDBY_RTSCTS=1`. No configuration file is required. The unpublished B1
physical-identity options (`--usb-carrier`, `--uart-carrier`, and related
environment variables) were removed before B2 rather than retained as aliases.

Unix listen/connect and inherited-fd carrier options remain test backends. Their
named standby forms are `--standby-carrier-listen NAME=PATH`,
`--standby-carrier-connect NAME=PATH`, and `--standby-carrier-fd NAME=FD`.

TTY setup is nonblocking raw 8N1 by default, with configurable baud and optional
RTS/CTS. Requested settings must survive termios readback or opening fails and
is retried. A standby TTY also requests `TIOCEXCL`; this rejects later opens but
does not prove that no reader already owns a descriptor.

## Ownership prerequisites

RAW requires its direct carrier owner (normally the supervisor) to be the sole
RX consumer. A dedicated DIRTY endpoint may use the same ownership model, but
a shared-console DIRTY endpoint instead makes `lmux-uart-dispatch` the sole
physical RX owner and connects the supervisor through local IPC. DIRTY permits
unrelated TX output on the wire; it is not a multiplexer for two readers.
`TIOCEXCL` and `/proc` diagnostics do not prove that no descriptor was opened
earlier and do not replace deployment ordering.

Deployment must disable competing getty/login/shell readers and any kernel
serial-input consumer before assigning RX ownership. RAW additionally requires
a clean, dedicated TX path with no printk or serial-console writer. DIRTY may
coexist with printk or serial-console TX only when no other kernel or userspace
path consumes RX. LMUX never changes console, getty, printk, or SysRq
configuration. Shared-console setup and recovery are specified in the
[dispatcher deployment guide](uart-dispatcher.md#shared-console-dirty-deployment).

## Standby behavior

Device presence is handled by independent reopen state. A missing primary does
not block standby polling, and a missing standby does not affect the primary or
the worker. Standby reads are bounded per poll iteration. Bytes are discarded
and never delivered to OUTER.

Only a valid, completed LMC1 transaction can make a standby endpoint active.
Text status keeps the legacy `carrier:` line and adds manager generation plus
named endpoint type, mode, role, state, path, serial settings, counters, and
ownership flags. No MCP switching tool or HC2 extension is added.

## LMC1 RAW record

LMC1 carries the exact bounded endpoint name and requested mode, not a local
array index or a USB/UART enum. A candidate is accepted only when the name
matches a locally configured endpoint exactly. Host and target device paths are
never compared or transmitted as identity.

RAW control uses a zero-delimited COBS record whose decoded magic is
`LMC1`; valid OUTER records decode to `LMO1`, so a complete valid OUTER record
cannot be mistaken for LMC1. A decoded LMC1 message is exactly 112 bytes:

```text
offset  size  field
0       4     "LMC1"
4       1     version = 1
5       1     message type
6       2     status, little-endian
8       8     carrier generation, little-endian
16      4     transaction deadline in ms, little-endian
20      16    host request nonce
36      16    target cookie
52      16    target boot ID
68      1     mode (1=raw, 2=dirty)
69      1     endpoint-name length
70      2     reserved zero
72      32    name bytes followed by zero padding
104     4     reserved zero
108     4     CRC32C of bytes 0..107
```

The COBS encoding plus delimiter is at most 114 bytes. Names use the carrier
name grammar and exact case-sensitive matching. Reserved bytes, padding,
version, type, mode, name, and CRC are validated before dispatch. A malformed
record with LMC1 magic is discarded; it is never offered to OUTER. A record
with other magic is offered to OUTER only on the authoritative endpoint.
Standby foreign records are discarded. Accumulation is bounded at 4352 bytes;
an oversize active RAW record is a protocol/carrier failure.

The implemented messages are `PREPARE`, `READY`, `COMMIT`, `COMMITTED`,
`QUERY`, `QUERY_REPLY`, `ERROR`, `RELEASE`, `RELEASED`, `RESUME`, and
`RESUMED`. Status values cover `BUSY`, unknown carrier, unsupported mode,
stale transaction, bad cookie, expired transaction, bad request, and
unavailable endpoint. Administrative release/resume uses the same decoded
LMC1 record and version; it does not alter OUTER/1 or LMUX/2.

The switch exchange is:

```text
PREPARE(request_nonce, observed_boot_id, observed_generation, candidate_name, mode)
READY(request_nonce, target_cookie, boot_id, generation, deadline)
COMMIT(request_nonce, target_cookie, boot_id, generation, candidate_name, mode)
COMMITTED(request_nonce, boot_id, new_generation, candidate_name, mode)
```

`request_nonce` and `target_cookie` are independently generated 128-bit values.
CRC and the cookie protect against accidental console traffic, not a malicious
peer with carrier access. One proposal may be pending. Duplicate messages for
the same transaction are idempotent; another proposal receives BUSY. A COMMIT
with an expired cookie, old boot ID, old generation, unknown name, or different
candidate/mode is rejected without changing authority.

The target retains a bounded record of the last two completed transactions
so duplicate COMMIT can receive the original COMMITTED result. A supervisor
restart creates a new boot ID and invalidates old proposals.

## Exact handover boundary

PREPARE and READY do not affect the old OUTER session. A partially written old
OUTER COBS record must finish before an LMC1 record is inserted on that physical
endpoint. If the old endpoint fails first, the partial record and old OUTER
session fail together; its prefix is never copied to the candidate.

The target commit point is the single assignment that increments generation
and changes authoritative endpoint/mode. If COMMIT arrives while a record on
the same physical fd has been partially written, the supervisor stops accepting
old OUTER input, finishes only that record, builds no subsequent old record,
and defers the assignment until the delimiter/CRLF boundary. At the commit
point:

1. input and output for the old OUTER are disabled;
2. queued but not-started old records are discarded; no partially written
   record is abandoned into the next codec;
3. the existing LMS1 LINK_DOWN barrier is started;
4. the old endpoint becomes control-only standby;
5. COMMITTED is cached and queued on the candidate;
6. COMMITTED is physically sent only after `LINK_DOWN_ACK` retires the old
   supervisor-worker link incarnation;
7. only after the complete COMMITTED record has been written is the one OUTER
   endpoint begun on the candidate, with target output held at a one-time
   host-adoption gate;
8. the first valid host OUTER HELLO opens that gate; the target then emits its
   HELLO/HELLO_ACK records and the ordinary OUTER handshake establishes the
   replacement incarnation;
9. OUTER establishment permits LMS1 LINK_UP and a fresh LMUX incarnation.

COMMITTED reports the target commit point, not OUTER, LMUX, or service
readiness. The CLI will report success only after new OUTER and LMUX
establishment and desired-service reconciliation.

RAW and DIRTY use the same transaction and authority boundary. Switching mode
on the same named fd still creates a new carrier incarnation, OUTER connection,
and LMUX incarnation. Old DIRTY fragments/ACKs and RAW decoder state are reset
at commit and cannot enter the replacement incarnation.

## Lost COMMITTED and ambiguous COMMIT

The host keeps old OUTER authoritative through READY. After sending COMMIT it
freezes old OUTER traffic and enters RESOLVING; it never resumes the old session
merely because COMMITTED timed out.

Within the caller's bounded deadline the host alternates QUERY and an identical
COMMIT on the candidate. Duplicate COMMIT receives cached COMMITTED. QUERY_REPLY
returns the current boot ID, authoritative endpoint name/mode, and generation.
The host accepts a committed result only for the queried boot ID, desired exact
name/mode, and a generation newer than the observed generation.

If the deadline expires before authority is known, the result is
`ambiguous-commit` and old OUTER remains frozen. If QUERY reports the candidate,
the host adopts it and starts a fresh OUTER. If the candidate committed but
OUTER/LMUX/reconciliation cannot converge, status is `committed-but-down`; there
is no automatic rollback. Switching back is another explicit transaction.

QUERY is read-only. It never retires, restarts, or advances an OUTER session.
This remains true for duplicates received before establishment and for late
duplicates received after reconciliation. When COMMITTED is lost, the target
can answer any number of QUERY retries while its replacement OUTER output is
held. A QUERY_REPLY which confirms the committed name, mode, boot ID, and newer
generation makes the host adopt that authority and emit a fresh OUTER HELLO.
That HELLO is the synchronization proof which releases target OUTER output.
Consequently no additional LMC1 synchronization message is required.

The gate is output-only: the target continues to parse bounded LMC1 QUERY and
duplicate COMMIT records, and a DIRTY adapter continues to emit its reliability
ACK/NACK traffic. A valid peer HELLO opens the gate once for that committed
generation. Subsequent QUERY records cannot close it. Physical writers still
serialize whole LMC1 and OUTER records; a control response waits for a partially
written OUTER record to reach its delimiter, while the pre-adoption gate starts
no OUTER record at all.

## Host CLI and observable completion

```text
lmux carrier status [--verbose] [--control SOCKET]
lmux carrier switch NAME [--mode raw|dirty] [--timeout-ms N] [--control SOCKET]
lmux carrier release NAME [--control SOCKET]
lmux carrier resume NAME [--control SOCKET]
```

The default mode is RAW and the default timeout is 30 seconds; accepted bounds
are 1 through 120 seconds. Switching is one nonblocking host `lmuxd` state
machine, not a sequence orchestrated by the CLI. A concurrent switch,
BOOT/deploy, or active FILE operation reports BUSY.

CLI success means target COMMIT, new OUTER establishment, a fresh LMUX
incarnation, and desired-service reconciliation have all completed. It never
means merely that COMMIT was written. Status reports active name/mode,
generation, named endpoint states, pending switch phase/name, OUTER/LMUX state,
last switch error, and `committed-but-down`. The legacy `carrier:` field remains.

## Administrative endpoint release and resume

`carrier release` is distinct from switching and from the dispatcher's
`console_released` state. Release applies only to a configured **standby**
endpoint. Releasing the authoritative active endpoint is rejected as
`ERR CARRIER RELEASE active-carrier` before its local fd is closed. This guard
prevents an operator from administratively releasing the only carrier that is
currently carrying the control transaction, including when it is the only
working UART.

For a valid standby release, the host first closes that local endpoint and
places it under an administrative hold, then sends the control transaction
over the still-authoritative active carrier. The target supervisor closes the
same inactive logical endpoint and replies only after its carrier manager has
applied the hold. While held, the endpoint is omitted from polling, accepting,
opening, and periodic reopen; its observable state is `released` and its fd is
closed. The active OUTER and LMUX connection IDs, carrier generation, worker,
PTY services, structured EXEC control, streams, and other services are not
retired or recreated.

The control exchange is:

```text
QUERY(nonce) -> QUERY_REPLY(target_boot_id, current_generation, active_name/mode)
RELEASE(same_nonce, target_boot_id, current_generation, subject_name)
    -> RELEASED(same transaction identity)

QUERY(nonce) -> QUERY_REPLY(...)
RESUME(same_nonce, target_boot_id, current_generation, subject_name)
    -> RESUMED(same transaction identity)
```

The exact bounded logical name is the subject; device paths and local table
indices are never transmitted. The messages use the RAW or DIRTY LMC1 codec of
the authoritative active endpoint, not the released subject endpoint. The
target rejects an old boot ID or generation as `STALE`, release of an active
subject or a concurrent handover as `BUSY`, and an unmatched name as
`UNKNOWN_CARRIER`. Resume is an idempotent no-op when the named endpoint is
already unheld, including when it is active. It retains the last two
administrative replies, so an identical request nonce can recover a lost
`RELEASED` or `RESUMED` reply without applying a second state transition.

Release and resume require compatible LMC1 administrative-message support in
the running host and target implementations. Matching carrier names and an
established older LMC1 switch implementation are not sufficient. A mixed
deployment which does not understand `RELEASE`/`RELEASED` and
`RESUME`/`RESUMED` cannot provide coordinated endpoint ownership and may end
with only the host-local state known.

Host-local application happens before remote confirmation. Therefore a lost
carrier or target rejection can produce an intentional partial result: status
reports the local hold and `target_admin=unknown`, and the CLI returns failure.
The target may already have applied the request when its confirmation is lost,
so this result is also operationally ambiguous: successful local release does
not imply successful target release, and `unknown` must not be reported as
`released`. There is no automatic rollback or carrier switch. Reissuing the
same operator action is an explicit operator decision; it starts a fresh
bounded transaction and converges idempotently. Read-only status remains
available while the transaction is pending.

The completion forms make that distinction explicit:

```text
OK CARRIER RELEASE name=primary local=released target=released generation=N
ERR CARRIER RELEASE partial name=primary local=released target=unknown REASON
```

`carrier resume` clears the administrative hold on both sides and permits the
ordinary configured open/listen/connect retry policy. Its success confirms the
removal of the hold, not that a physically absent device has opened. Resume
does not change carrier authority or generation and does not switch back to
that endpoint. A subsequent explicit `carrier switch` is required.

Status deliberately separates administrative policy from transport health and
from console ownership:

| Status field | Meaning |
| --- | --- |
| `state=released`, `administratively_released=yes`, `fd=-1` | The local administrative hold is applied; reopen is disabled. |
| `state=down` or `state=failed`, `administratively_released=no` | The endpoint is not held; normal open/reopen policy remains eligible. |
| `target_admin=released` or `target_admin=resumed` | The current target supervisor confirmed that administrative state. |
| `target_admin=unknown` | No current target confirmation exists; local state alone is not proof of target state. |
| `console_released=yes` | The separate DIRTY console-handoff hold is active; this is not administrative release. |
| `carrier_admin: phase=...` and `last_result=...` | Current transaction progress and the last complete or partial outcome. |

Closing `/dev/ttyACM0` on the host and `/dev/ttyGS0` on the target releases
LMUX ownership only. It does not unbind a USB gadget, change the USB controller
role, remove either device node, or alter USB enumeration. Once both LMUX fds
are closed, another application may open the endpoints subject to the platform
and device-driver ownership rules.

Administrative holds are deliberately process-local and are not written to
disk. Restarting either daemon reconstructs endpoint policy from its startup
configuration, so that side is no longer known to be released. An OUTER peer
restart clears host target-confirmation status. If release must remain policy
after a supervisor or host-daemon restart, platform startup must configure that
policy or the operator must issue `carrier release` again after reconciliation.
A previously successful confirmation must not be treated as current after
either endpoint-owning process restarts.

Restoring a released primary is two explicit operations: resume ownership
first, verify that the endpoint can reopen, and then request a separate carrier
switch. Resume alone never changes the authoritative carrier:

```sh
lmux carrier switch rescue --mode dirty
lmux carrier release primary
lmux carrier status

# Primary is now released on both host and target.
# OUTER/LMUX continue to run through rescue.

lmux carrier resume primary
lmux carrier switch primary --mode raw
```

### Hardware acceptance record

The coordinated release contract was exercised on a Hantek DSO2D15 with
`1.0.2+build.966`. Host `/dev/ttyACM0` and target `/dev/ttyGS0` were both
released, both LMUX-owned fds closed, and neither automatic reopen loop resumed
ownership during the observation window. USB CDC ACM remained available for
unrelated applications. The active rescue/DIRTY carrier, OUTER and LMUX
incarnations, managed worker, services, and structured EXEC remained usable.

This is a historical hardware-acceptance result, not a requirement that LMUX
be deployed on Hantek hardware.

## B2 fixed memory bounds

There are still at most two endpoints and one OUTER instance. Each endpoint has
one RAW demultiplexer containing a 4352-byte receive record and a 114-byte LMC1
transmit record; only one transmit record can be queued per endpoint. The target
has one pending transaction and a two-entry completed-transaction cache. There
are no per-request heap allocations, unbounded control queues, or control
threads. Measured on the ARM EABI5 soft-float build, one RAW I/O state is 4512
bytes (9024 bytes for both endpoints), the pending state is 120 bytes, and the
two-entry completed cache is 208 bytes. Including the boot ID and small scalar
state, target B2 carrier-control storage is 9376 fixed bytes; the complete
supervisor structure is 53840 bytes. `lmux_lmc1_msg` is 104 bytes in memory
(the wire representation remains exactly 112 bytes).

The later administrative release/resume extension adds a separate bounded
two-entry completed-reply cache plus its index (212 bytes on ARM) so lost
`RELEASED`/`RESUMED` can be replayed without consuming or evicting switch
COMMIT history. It adds no per-transaction allocation or queue; the endpoint
administrative flags are included in the current manager sizes above.

## DIRTY/1 grammar

Every record has this exact outer grammar:

```text
"~LMUX-D1:" canonical-base64(binary-body) "\r\n"
```

The prefix is searched as a byte sequence and does not need a console line
boundary. The parser retains at most 316 base64 bytes. Bytes before a prefix are
foreign output and are discarded. A new `~LMUX-D1:` prefix abandons an
incomplete candidate and begins a new one. Bare LF, a non-base64 byte, bad
padding, an oversized candidate, or anything other than CRLF at the end makes
the whole candidate invalid. Inserting even valid base64 characters inside a
candidate changes its record CRC and invalidates it. The decoder never exposes
payload before all structural checks and CRCs pass.

The decoded binary body is little-endian:

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 4 | magic `LMD1` |
| 4 | 1 | version, currently 1 |
| 5 | 1 | type: DATA=1, ACK=2, NACK=3, FAIL=4 |
| 6 | 1 | header length, currently 40 |
| 7 | 1 | flags, zero in version 1 |
| 8 | 8 | DIRTY sender session ID |
| 16 | 4 | packet sequence |
| 20 | 2 | complete packet length |
| 22 | 2 | fragment byte offset |
| 24 | 1 | fragment index |
| 25 | 1 | fragment count |
| 26 | 1 | missing-fragment bitmap |
| 27 | 1 | FAIL reason |
| 28 | 2 | fragment payload length |
| 30 | 2 | reserved, zero |
| 32 | 4 | CRC32C of the complete packet |
| 36 | 4 | CRC32C of the fragment payload |
| 40 | 0..192 | fragment payload |
| 40+payload | 4 | CRC32C of header and payload |

The maximum binary body is 236 bytes and the maximum complete ASCII record is
327 bytes. ACK, NACK, and FAIL have no payload. ACK has no bitmap; NACK has a
nonzero six-bit missing bitmap; FAIL has one defined failure reason. Reliability
control is a DIRTY namespace and is not LMC1. DIRTY session IDs and packet
sequences are also independent of OUTER connection IDs/frame sequences and of
LMUX incarnations.

The current implementation limits a packet to 1024 bytes, a fragment to 192
bytes (six fragments), and each TX and RX window to two packets. Sizes and
offsets are explicit on wire so a later negotiated revision can choose smaller
bounds; peers must use the version-1 limits until such negotiation exists.

### ACK, retransmission, and ordered delivery

The two TX slots are the retransmit cache. An outstanding packet is never
evicted or overwritten: submission returns `EAGAIN` when both slots are in use,
and a slot is released only by a valid ACK for the local DIRTY session and exact
packet sequence.

ACK is deliberately **ACK-on-consume**. A receiver ACKs only after the complete
packet and packet CRC validate, the packet is the next ordered packet, and its
bytes have been consumed by the adapter owner. Thus ACK transfers responsibility
past the DIRTY RX buffer instead of merely promising that a stalled two-slot
buffer will retain data indefinitely. A lost ACK causes a duplicate DATA packet;
because `rx_next` advances only after consumption, every same-session sequence
behind `rx_next` is safely ACKed again and is never delivered twice.

An incomplete packet causes a NACK containing only its missing fragment bitmap.
A complete packet with a bad packet CRC discards all collected bytes and NACKs
all fragments. NACK retransmits only selected cached fragments. Missing ACK
causes a full-packet retransmission. Packet 1 may be assembled while packet 0 is
incomplete, but only the verified contiguous packet at `rx_next` is exposed.
There is no skip operation.

There are at most five retransmit/NACK rounds. The retry interval is derived
from baud, the maximum record size, and six-fragment packet transmission time,
with bounded 200 ms and 5 s limits. Every slot also has an absolute 30-second
deadline. Retry exhaustion, deadline expiry, RX-window overflow, contradictory
metadata, a peer FAIL, or physical peer disappearance is an explicit adapter
failure. The owner must retire the associated OUTER incarnation; there is no
rollback or LMUX recovery mechanism inside DIRTY.

Stale or foreign session records are discarded. Changing session identity is
an explicit adapter reset operation, never inferred from serial input. CRC is
corruption detection, not authentication.

### DIRTY resource bound

The B3 ARM EABI5 soft-float build measured `struct lmux_dirty` at 5104 bytes.
B4 session counters and the B4.2 bounded parser diagnostic bring the current
integrated structure to 5400 bytes. The diagnostic retains only per-reason
counters, the most recent valid record metadata, and one 32-byte invalid and
foreign sample; it has no growing history. The structure includes both
1024-byte TX cache slots, both 1024-byte RX slots, the parser, one maximum
encoded output record, three bounded reliability-control slots, and statistics.
There are no per-packet heap allocations. On the ARM EABI5 build, `pahole`
confirms 5400 bytes, the standalone DIRTY object has 9625 bytes of text and no
data/BSS, and `-fstack-usage` reports a 656-byte maximum frame in
`lmux_dirty_feed`. The fixed-state-plus-adapter-frame bound is therefore 6056
bytes, excluding its owner's stack/state. The complete supervisor main frame is
63,080 bytes in this static build (up from the previously documented 60,888-byte
B4 measurement); target `lmuxd` has 12,208 bytes of BSS. The supervisor still
owns exactly one adapter for the authoritative DIRTY session, not one per
endpoint.

## DIRTY-safe LMC1 bootstrap and integration (B4)

The decoded 112-byte LMC1 message and its CRC are unchanged. On a candidate
requested in DIRTY mode, the complete existing COBS LMC1 record, including its
zero delimiter, is carried in this independent envelope:

```text
"~LMUX-C1:" canonical-base64(COBS-LMC1-record-with-zero) "\r\n"
```

The maximum COBS message is 114 bytes, the base64 body is at most 152 bytes,
and the complete bootstrap record is at most 163 bytes. The parser has a fixed
152-byte body, searches the prefix without assuming line alignment, rejects
non-canonical base64, requires the decoded trailing zero, and then applies the
unchanged LMC1 version/name/reserved-field/CRC validation. A corrupt or
truncated envelope is discarded and the existing bounded LMC1 retry/QUERY
state machine retransmits the logical message. `~LMUX-C1:` is distinct from
DIRTY reliability records (`~LMUX-D1:`), and bootstrap messages never enter the
ordered OUTER byte namespace.

DIRTY PREPARE/READY/COMMIT/COMMITTED/QUERY use this envelope even before a data
session exists. RAW transactions retain the zero-delimited COBS representation.
Replies use the codec on which the request was validated. COMMITTED is fully
written using that bootstrap codec after LMS1 LINK_DOWN_ACK; only then is the
new data adapter allowed to emit its first record and OUTER begins.

On an endpoint that is currently RAW, a DIRTY bootstrap prefix is recognized
only at a COBS record boundary. The sender likewise serializes the envelope
between complete physical records. This prevents an accidental ASCII prefix
inside arbitrary encoded OUTER data from stealing bytes from the RAW decoder;
standby and already-DIRTY endpoints still search and resynchronize among
arbitrary foreign bytes.

Each endpoint has lightweight bounded RAW and DIRTY-bootstrap decoders so a
standby can recognize either requested mode. There is still only one 5160-byte
DIRTY data adapter. Its two directional 64-bit session IDs are deterministically
derived from target boot ID, committed carrier generation, exact carrier name,
and direction. They are a separate namespace and are not authentication; a new
generation necessarily produces a new pair. Query recovery therefore derives
the same IDs even when COMMITTED was lost.

OUTER encoded bytes are copied into DIRTY packets in chunks of at most 1024
bytes. Received packets are offered to the single OUTER endpoint only in the
verified contiguous order and are ACKed only after `lmux_link_input_bytes` has
consumed the complete chunk. A DIRTY retry/deadline/protocol failure invokes the
normal carrier/OUTER down path and LMS1 LINK_DOWN without replacing the worker.
The committed carrier remains authoritative and status reports
`committed-but-down`; there is no automatic rollback or fallback.

The complete ARM supervisor state is 59864 bytes in B4, 6024 bytes above the
53840-byte B2 measurement. This delta includes the one DIRTY adapter, two
bounded bootstrap parsers, codec/session state, and the deferred-COMMIT record.
`-fstack-usage` reports 60888 bytes for `lmux_supervisor_main` (the state is its
dominant local object); the largest independent DIRTY parsing frame remains
640 bytes. The final statically linked target `lmuxd` has 741436 bytes of text, 13892
bytes of data, and 12208 bytes of BSS. Standby endpoints do not each allocate a
DIRTY data adapter.

Status adds a current-session `carrier_dirty:` line with session/sequence,
record, fragment, delivery, CRC, retransmission, ACK/NACK, duplicate/stale,
retry/deadline and last-failure fields. The structure is reset at each DIRTY
incarnation, so old-session errors are not attributed to the current session.
