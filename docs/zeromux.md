# Zeromux

Zeromux is the host-side first-contact, bootstrap and installation subsystem for targets where LMUX is not yet available.

It exists specifically for the state:

```text
host
  |
  | bootstrap channel
  v
target shell
  |
  | no OUTER
  | no supervisor
  | no LMUX
  v
bare / unmanaged target
```

Zeromux brings such a target to a state where a normal LMUX supervisor can run.

Zeromux is **not** an alternative LMUX transport and is not part of the normal LMUX data path after installation.

## 1. Architectural boundary

The important distinction is:

```text
bootstrap channel
    !=
operational LMUX carrier
```

The bootstrap channel is how Zeromux talks to the target before LMUX exists.

The operational LMUX carrier is the channel the installed supervisor will use for OUTER/1.

They may be different:

```text
bootstrap:
    HOST /dev/ttyUSB0
        <- UART ->
    TARGET /dev/ttyS0

LMUX:
    HOST /dev/ttyACM0
        <- USB gadget serial ->
    TARGET /dev/ttyGS0
```

They may also be the same physical channel:

```text
bootstrap UART
    -> shell
    -> Zeromux install
    -> explicit ownership handoff
    -> OUTER/1
    -> LMUX
```

Zeromux must support both models.

## 2. No blind target mutation

Read-only discovery and target mutation are separate operations.

A probe must never install, upload, overwrite files, kill services or start LMUX.

Installation requires an explicit user command.

High-level model:

```text
probe
    ↓
compatibility information
    ↓
explicit install
    ↓
artifact transfer
    ↓
verified target installation
    ↓
LMUX launch
```

Zeromux must not be designed as a command that an engineer can blindly run against an unknown serial port and hope that it installs LMUX.

The bootstrap endpoint is always explicitly selected by the user.

## 3. CLI model

Initial CLI namespace:

```text
lmux zeromux probe ...
lmux zeromux install ...
```

Exact options may evolve, but the architectural semantics below are normative.

### Probe

Example:

```text
lmux zeromux probe \
    --tty /dev/ttyUSB0 \
    --baud 115200
```

The bootstrap device is mandatory.

Serial settings may include:

```text
--baud N
--data-bits 5|6|7|8
--parity none|even|odd
--stop-bits 1|2
--flow none|rtscts|xonxoff
```

Reasonable serial defaults may exist, for example 8N1/no-flow, but Zeromux must never guess the physical bootstrap device.

Z0 implements only the read-only probe form:

```text
lmux zeromux probe --tty DEVICE \
    [--baud N] \
    [--data-bits 5|6|7|8] \
    [--parity none|even|odd] \
    [--stop-bits 1|2] \
    [--flow none|rtscts|xonxoff] \
    [--timeout-ms N]
```

Defaults are:

```text
baud=115200
data-bits=8
parity=none
stop-bits=1
flow=none
```

Z0 opens only the explicitly selected bootstrap tty. It does not use the
host `lmuxd` control socket, OUTER, LMUX/2, or any target supervisor.

### Z1 low-level send

Z1 adds a deliberately low-level sender validation command:

```text
lmux zeromux send LOCAL_FILE TARGET_TEMP \
    --tty DEVICE \
    [serial options]
```

This command is not install. It opens the selected pre-authenticated
bootstrap shell, probes the needed sender capabilities, transfers the local
file to the explicitly supplied temporary target pathname, and verifies the
received bytes. It does not chmod a final destination, rename into place,
launch LMUX, or alter supervisor/worker state.

### Install with a distinct future LMUX carrier

Example:

```text
lmux zeromux install ./lmuxd \
    --tty /dev/ttyUSB0 \
    --baud 115200 \
    --install-dir /root/zmx-lmux \
    --lmux-carrier /dev/ttyGS0
```

Z2 requires all three explicit target choices:

```text
--tty BOOTSTRAP_DEVICE
--install-dir TARGET_DIR
--lmux-carrier TARGET_CARRIER
```

The installed executable path is:

```text
<TARGET_DIR>/lmuxd
```

`--lmux-carrier` describes the target-side operational LMUX carrier.

It is not the host bootstrap tty.

Zeromux must not infer a host-side operational carrier such as `/dev/ttyACM0` merely from a target path such as `/dev/ttyGS0`.

Host-side LMUX carrier configuration remains a separate host/runtime concern.

Z2 launches the installed supervisor on the distinct carrier but does not verify
OUTER, LMUX/2 or service reconciliation. It also does not install persistent
respawn policy: no `/etc/inittab`, no systemd units and no embedded process
manager are modified. The bootstrap UART remains an authenticated shell channel
after launch.

### Install using the bootstrap channel itself

`--lmux-carrier` is optional.

If omitted:

```text
effective LMUX carrier = bootstrap channel
```

This does **not** mean Zeromux guesses a target `/dev/tty*` pathname.

It means Zeromux must perform an explicit ownership handoff of the already established bootstrap byte channel from the target shell/bootstrap session to the newly started LMUX supervisor.

Conceptually:

```text
HOST Zeromux
    |
    | UART bytes
    v
target shell
    |
    | install completed
    | shell/bootstrap relinquishes channel
    v
target supervisor
    |
    | same ordered full-duplex bytes
    v
OUTER/1
```

Only one owner may consume the channel after handoff.

A getty, shell and supervisor must not concurrently read/write the same UART.

If the current target environment does not provide a safe handoff mechanism, install must fail clearly and request an explicit operational LMUX carrier rather than guessing.

## 4. Same-channel handoff

Same-channel operation is a first-class Zeromux mode.

Preferred implementation should reuse the existing carrier abstraction and inherited-fd support where possible.

The implementation must investigate the actual shell/session state rather than assuming:

* stdin is a writable tty;
* fd 0 is opened read/write;
* `/dev/tty` is available;
* `tty`, `readlink` or `stty` applets exist;
* a getty will or will not respawn;
* the target pathname of the bootstrap tty can be inferred safely.

A possible safe target-specific strategy may be:

```text
bootstrap shell
    ↓
prepare installed lmuxd
    ↓
replace shell with lmuxd via exec
    ↓
lmuxd inherits the already-open full-duplex tty fd
```

but this is not a universal assumption and must be validated by probe/session capabilities.

If an inherited bootstrap fd is used as the LMUX carrier, normal shell stdout/stderr must no longer contaminate the byte stream after handoff.

The first bytes after handoff must belong to the expected OUTER/1 lifecycle, not shell prompts, banners or diagnostics.

## 5. Internal source layout

Zeromux is a real multi-unit subsystem and should live under:

```text
src/host/zeromux/
```

Expected units:

```text
zeromux.c
zeromux.h

probe.c
probe.h

session.c
session.h

sender.c
sender.h

install.c
install.h
```

Additional files are justified when a real multi-unit backend appears, for example a transfer protocol implementation.

Do not create one directory per sender backend unless a backend genuinely needs multiple compilation units.

Suggested responsibilities:

### `zeromux.[ch]`

High-level orchestration and shared state.

Does not implement shell parsing, transfer protocols or installation policy directly.

### `session.[ch]`

Bootstrap byte/session discipline:

* bootstrap carrier ownership;
* bounded input/output;
* command execution;
* prompt/sentinel synchronization;
* timeouts;
* shell quoting;
* interruption;
* channel handoff preparation.

No target capability policy.

### `probe.[ch]`

Read-only target discovery.

Produces structured capability information.

Must not mutate target state.

Z0 probe uses a pre-authenticated shell session. It proves shell usability
with nonce-bounded sentinel command transactions, not by matching prompts.
Each command has unique begin/end markers:

```text
ZMX_BEGIN_<nonce>
...
ZMX_END_<nonce>_<status>
```

Bytes outside the matching markers are treated as echo, prompt or console
noise and are not command output. A stale or mismatched marker cannot
complete the current transaction.

### `sender.[ch]`

Artifact delivery selection and common sender interface.

Backends are selected from capabilities actually observed by probe.

### `install.[ch]`

Installation policy:

* select destination;
* choose compatible artifact;
* transfer;
* verify;
* place final executable;
* launch supervisor;
* optionally hand bootstrap channel to LMUX.

Does not implement raw transfer protocols itself.

## 6. Reuse the carrier abstraction

Do not invent a second generic byte transport specifically for Zeromux.

Bootstrap communication needs the same fundamental abstraction:

```text
ordered full-duplex bytes
```

Existing carrier infrastructure should be reused where appropriate.

UART configuration belongs to tty carrier configuration, not to Zeromux shell logic.

This may require extending tty carrier configuration to support:

```text
baud
data bits
parity
stop bits
RTS/CTS
software flow control
```

Such configuration must remain reusable outside Zeromux.

Zeromux itself operates above the bootstrap carrier.

## 7. Probe contract

Probe is read-only.

It should discover facts required for compatibility, transfer and installation decisions.

Candidate information includes:

```text
system:
    kernel / uname information
    architecture
    endianness where relevant
    ELF machine / ABI
    EABI generation
    floating-point ABI
    libc/runtime information where relevant

shell:
    usable shell
    shell command semantics required by Zeromux
    command synchronization capability

tools:
    rz
    rx
    rb
    sz
    sx
    sb
    dd
    stty
    base64
    uudecode
    uuencode
    xxd
    printf
    cat
    chmod
    mv
    cp
    rm
    sync
    sha256 tools where available

filesystem:
    candidate writable locations
    candidate executable locations
    free space where cheaply discoverable

bootstrap:
    selected host endpoint
    serial configuration
    capabilities required for same-channel handoff

requested LMUX carrier:
    existence/properties if explicitly requested
```

This is not an exhaustive mandatory-command list.

Most importantly:

**BusyBox applets must never be assumed merely because BusyBox exists.**

Every command/capability Zeromux intends to use must be probed or replaced by a mechanism known to be available.

## 8. Probe techniques must be conservative

Probe should prefer commands and constructs that do not mutate persistent target state.

Temporary operations may be considered only when necessary to determine writable/executable filesystem properties, and should be explicitly scoped and cleaned up.

Probe should distinguish:

```text
known yes
known no
unknown
```

instead of converting every inability to probe into `no`.

Z0 reports `unknown` when the probe mechanism itself cannot establish a
fact. Missing optional commands do not fail the whole probe.

### Hantek Z0 observation

The first hardware Z0 probe was run manually against a pre-authenticated
Hantek shell on host `/dev/ttyUSB0` at `115200 8N1 flow=none`.

Observed facts included:

```text
shell=usable
authentication=pre-authenticated
machine=armv5tejl

rz=yes
rx=no
rb=no
sz=yes
sx=no
sb=no

dd=yes
stty=yes
base64=no
uudecode=yes
uuencode=yes
xxd=no

sha256sum=yes
```

These are profile observations, not generic Zeromux assumptions.

Do not infer target compatibility from host assumptions.

## 9. Compatibility profile

Probe output may be represented internally as a compatibility profile.

The profile is a cache/hint, not permanent truth.

Configuration precedence:

```text
explicit user config
    >
saved profile
    >
automatic probe
```

Explicit configuration always wins.

Profiles may contain facts such as:

```text
architecture
ABI
available transfer methods
candidate install locations
shell capabilities
serial/session capabilities
```

A saved profile must not silently override a newly explicit bootstrap or LMUX carrier selection.

## 10. Artifact selection

Zeromux must select target artifacts by compatibility, not by target name alone.

Example profile:

```text
arch=arm
machine=ARM
eabi=5
float_abi=soft
static=yes
```

For the current Hantek baseline this corresponds to the existing ARMv5/EABI5 soft-float static target artifact.

Do not hardcode Hantek-specific compatibility logic into the generic Zeromux core.

Explicit artifact selection is allowed and has priority over automatic selection.

## 11. Sender architecture

Sender is a delivery mechanism, not a transport for normal LMUX operation.

Potential sender backends include:

```text
ZMODEM
YMODEM
XMODEM
base64/text decoding
uudecode
xxd
shell-safe encoded fallback
```

The presence of a protocol name in this document does not mean every backend must be implemented immediately.

Sender selection is capability-driven:

```text
probe results
    ↓
available sender backends
    ↓
policy selects suitable backend
```

No backend may be selected based on an assumed BusyBox applet set.

### Common sender responsibilities

A sender should expose semantics equivalent to:

```text
prepare
send bounded bytes
finish
abort
```

Installation code should not care whether bytes were transported by YMODEM or a textual fallback.

Transfer implementations must be binary-clean where applicable.

X/Y/ZMODEM are bootstrap delivery mechanisms only.

They never become carrier, OUTER or LMUX protocols.

### Z1 sender: host uuencode -> target uudecode

The first implemented backend is selected only when the structured probe says:

```text
uudecode=yes
stty=yes
sha256sum=yes
```

The host performs classic uuencode itself; it does not shell out to a host
`uuencode` program and it does not require target-side `uuencode`. The target
side runs foreground `uudecode` directly on the bootstrap tty/stdin. The shell
first disables target-side echo with `stty -echo`, emits a nonce-bounded READY
marker as a complete line, starts `uudecode`, then keeps ownership of stdin
until it reads a separate `~ZMX_DATA_END_<nonce>` line. After that drain barrier
it restores echo and emits the matching nonce-bounded END/status marker as a
complete line. READY and END tokens are never recognized as substrings of echoed
commands, prompts or diagnostics.

Interactive-shell heredocs are explicitly unsuitable for bulk Zeromux transfer:
hardware testing on the Hantek target showed PS2 prompt output (`>`) and target
tty echo interleaved with encoded data when `uudecode <<...` was used. Bulk
sender backends must let the foreground receiver own bootstrap stdin during the
DATA phase instead of routing the payload through the shell parser.

The wrapper must also protect the interactive shell from early decoder failure.
If `uudecode` fails before consuming all encoded lines, the wrapper saves the
decoder status and discards bootstrap input until the exact DATA_END line. This
prevents remaining uuencoded payload from becoming shell commands, restores echo
when the link is still alive, and reports the saved decoder status in END.
Carrier loss during an active foreground decoder can still require manual shell
recovery because the host cannot prove target tty state after the physical link
is gone.

Sender timeouts are progress/stall based. `--timeout-ms` bounds inactivity for
setup, DATA progress and final status, but a healthy bulk transfer may exceed a
normal shell-command wall-clock deadline. At 115200 8N1, 1 MiB of raw data is
about 1.38 MiB after classic uuencode and can take more than two minutes on the
wire before protocol overhead and target latency.

The uploaded target file is verified with target `sha256sum`. If verification
cannot be performed, Z1 reports the sender as unsupported rather than claiming
probable success.

ZMODEM remains a future optimized sender backend. `rz=yes` is recorded by
probe but is not used by Z1.

Z1 has been hardware-verified on the Hantek target over `/dev/ttyUSB0` at
115200 8N1 with a 1 MiB transfer verified by target `sha256sum`.

### Z2 explicit install + distinct-carrier launch

Z2 adds an explicit mutating install command:

```text
lmux zeromux install FILE \
    --tty BOOTSTRAP_DEVICE \
    --install-dir TARGET_DIR \
    --lmux-carrier TARGET_CARRIER \
    [serial options]
```

The install directory is never guessed. The operational LMUX carrier is the
target-side path passed to the launched supervisor and is never mapped to a
host-side carrier by Zeromux.

The target-side flow is:

```text
mkdir -p TARGET_DIR
    ↓
if TARGET_DIR/lmuxd already has the local SHA256, skip upload
    ↓
upload to TARGET_DIR/.lmuxd.zmx.<nonce>.tmp
    ↓
verify temp by SHA256
    ↓
chmod 0755 temp
    ↓
mv -f temp TARGET_DIR/lmuxd
    ↓
sync
    ↓
verify final TARGET_DIR/lmuxd by SHA256
    ↓
launch TARGET_DIR/lmuxd --supervisor --outer --carrier TARGET_CARRIER
```

The temporary file is in the same directory as the final executable so the final
move is pathname-atomic on the target filesystem. Existing `TARGET_DIR/lmuxd`
is not removed before a verified temporary artifact is ready. If launch fails
after the final move, Z2 reports launch failure and leaves the installed
artifact in place; it does not attempt rollback.

The launched supervisor is detached from the bootstrap UART:

```text
TARGET_DIR/lmuxd --supervisor --outer --carrier TARGET_CARRIER \
    </dev/null >/tmp/lmux-zeromux-<nonce>.log 2>&1 &
```

Z2 captures `$!` for the backgrounded supervisor process, then uses host-side
monotonic timing to check `kill -0 PID` repeatedly through a bounded startup
grace window. `status=started` means only that this exact PID survived the
initial grace period. It catches immediate startup failures such as carrier-open
errors, but it does not prove OUTER, LMUX/2 or service health. Normal LMUX
tooling remains responsible for operational connection verification.

Z2 does not implement same-channel handoff. The bootstrap UART remains a shell
channel after install/launch. Same-channel bootstrap-to-LMUX transition is a
future Z3 concern.

## 12. Installation contract

Installation is explicit.

High-level flow:

```text
obtain/probe compatibility
    ↓
choose compatible artifact
    ↓
choose destination
    ↓
transfer to temporary target file
    ↓
verify received artifact where possible
    ↓
install final lmuxd
    ↓
configure launch
    ↓
start supervisor
```

Installation must not overwrite the active target executable until transfer and available integrity checks have succeeded.

Temporary and final paths must be distinct where the target filesystem permits atomic replacement.

Do not assume `/root/lmux`.

A destination may be explicitly configured or selected from probed suitable locations.

## 13. LMUX carrier selection at install time

There are two modes.

### Explicit distinct carrier

User supplies:

```text
--lmux-carrier TARGET_SPEC
```

The installed supervisor is configured to use that target carrier.

The bootstrap session remains separate.

Zeromux must not invent a host-side mapping for that target carrier.

### Same bootstrap channel

No `--lmux-carrier` is supplied.

Zeromux requests same-channel operation.

Installation succeeds into operational LMUX only if Zeromux can establish a safe ownership handoff from bootstrap session to supervisor.

If it cannot, it must report that same-channel launch is unsupported for the discovered environment.

It must not silently select another target device.

## 14. Carrier ownership invariant

At any instant a physical bootstrap channel has one logical owner.

Typical transition:

```text
ZEROMUX_SHELL
    ↓
HANDOFF
    ↓
OUTER
```

Never:

```text
shell/getty ──┐
              ├── same UART
lmuxd OUTER ──┘
```

The handoff mechanism must have a clear byte boundary.

No shell output generated after the boundary may be interpreted as OUTER input.

No OUTER bytes may be consumed by the old shell/session parser.

## 15. Failure semantics

Before LMUX launch, failures remain Zeromux/bootstrap failures.

Examples:

```text
probe timeout
unexpected shell response
sender unavailable
transfer failure
verification failure
destination not writable
destination not executable
unsafe same-channel handoff
```

Such failures must not be disguised as LMUX/OUTER failures.

After successful channel handoff, the bootstrap session is over and normal OUTER lifecycle begins.

Zeromux must not attempt to parse OUTER as shell traffic.

## 16. No blind persistence assumptions

A path being writable does not imply it survives reboot.

A path surviving reboot does not imply it is executable.

Probe should report known properties separately.

Automatic install-location selection, when eventually implemented, should prefer known writable+executable candidates but should not claim persistence unless it has evidence.

The engineer may explicitly override automatic selection.

## 17. Security/trust scope

Initial Zeromux integrity is operational, not an authenticity system.

Do not invent signatures or PKI as part of the first implementation.

SHA256 may be used where target capabilities permit it, but lack of a target SHA256 utility should not automatically make all bootstrap impossible if another verified delivery mechanism exists.

Authenticity/signing is separate future work.

## 18. Interaction with normal LMUX deployment

Zeromux is for first contact or recovery when normal LMUX is unavailable.

Once a working supervisor/OUTER/LMUX exists, normal updates use:

```text
lmux deploy FILE
```

not Zeromux.

Conceptually:

```text
no LMUX
    -> Zeromux

working LMUX
    -> normal deploy
```

Do not make Zeromux a second ongoing deployment stack.

## 19. Future UART relay / nested targets

The architecture must allow the bootstrap byte channel to come from something other than a local host tty.

Future topology:

```text
HOST
  |
  | LMUX to target A
  v
target A UART relay
  |
  | bootstrap bytes
  v
bare target B
```

For Zeromux core:

```text
local tty bootstrap carrier
        and
relay-provided bootstrap carrier
```

should eventually present equivalent ordered full-duplex bootstrap bytes.

`probe`, `sender` and `install` should not need to know whether those bytes originate from a local UART or a relay.

Do not implement relay in initial Zeromux milestones.

## 20. Development milestones

Zeromux should be implemented incrementally.

### Z0 — subsystem skeleton + bootstrap session + read-only probe

Implement:

```text
src/host/zeromux/
```

and:

```text
lmux zeromux probe ...
```

Scope:

* tty bootstrap channel;
* explicit serial configuration;
* shell/session synchronization;
* read-only target discovery;
* structured probe result;
* no artifact transfer;
* no install;
* no target mutation beyond narrowly justified temporary capability probes.

Primary goal: prove reliable shell automation over the real bootstrap UART.

### Z1 — sender abstraction + first real delivery backend

Based on actual Z0 hardware probe results, choose the simplest reliable sender available on the target.

Do not pre-implement every theoretical protocol.

Implement:

```text
sender common interface
first sender backend
transfer tests
abort/cleanup
```

Still no automatic LMUX installation unless needed for isolated sender validation.

### Z2 — explicit install on a distinct LMUX carrier

Implement:

```text
lmux zeromux install FILE
    --tty BOOTSTRAP_DEVICE
    --install-dir TARGET_DIR
    --lmux-carrier TARGET_PATH
```

Scope:

* artifact transfer;
* target temp/final install;
* executable setup;
* explicit supervisor launch;
* bootstrap UART remains a shell after launch;
* no persistent init/respawn configuration;
* no same-channel handoff;
* no automatic guessing of LMUX carrier.

### Z3 — same-channel bootstrap -> LMUX handoff

Implement install without `--lmux-carrier`:

```text
bootstrap channel
    -> install
    -> ownership handoff
    -> OUTER
```

This milestone must explicitly solve shell/getty/fd ownership and clean byte-boundary issues.

Do not call it complete merely because `lmuxd` can open the same `/dev/tty*` pathname.

### Z4 — profiles / artifact compatibility UX

Add reusable probe profiles and artifact selection policy:

```text
explicit config > saved profile > auto probe
```

Keep profile as cache/hint.

Do not make stale profile data authoritative.

### Z5 — relay-capable bootstrap source

Only after UART relay exists, permit Zeromux bootstrap bytes to arrive through target A.

Do not turn Zeromux or relay into a generic tunneling framework.

## 21. Testing philosophy

Zeromux introduces interaction with highly variable shell environments.

Tests should focus on explicit state-machine boundaries rather than reproducing every possible embedded shell.

Useful test layers:

```text
unit:
    parsing
    quoting
    probe result interpretation
    sender selection

pseudo-target/process integration:
    fragmented reads
    delayed prompts
    command echo
    CR/LF variants
    missing tools
    command failures
    partial sender failure
    disconnect

hardware:
    actual target UART
    actual shell
    actual discovered tools
    actual sender backend
```

Do not add giant fault matrices before a concrete failure boundary exists.

Hardware remains manual unless explicitly requested.

## 22. Non-goals for initial Zeromux

Do not make Zeromux:

* a replacement for normal LMUX deployment;
* a generic remote shell framework;
* a generic tunneling framework;
* a permanent UART protocol;
* an OUTER implementation;
* an LMUX implementation;
* a package manager;
* a hardware auto-discovery system that blindly scans ports;
* a mechanism that guesses the future LMUX carrier;
* dependent on assumed BusyBox applets.

No kernel modules.

No `n_gsm`.

No USB networking.

## 23. Core invariants

Keep these distinctions explicit:

```text
bootstrap carrier
    !=
OUTER carrier lifetime

Zeromux shell/session
    !=
OUTER session

sender
    !=
LMUX transport

install location
    !=
storage-root necessarily

bootstrap endpoint
    !=
future LMUX carrier necessarily

probe
    !=
install

profile
    !=
truth

artifact
    !=
running supervisor
```

And for same-channel operation:

```text
bootstrap bytes
    -> explicit ownership handoff
    -> OUTER bytes

never concurrent shell + OUTER ownership
```


## Bootstrap shell authentication

Initial Zeromux versions do not implement login or credential handling.

The bootstrap channel must already contain an authenticated interactive shell owned by the intended target user.

Typical workflow:

```text
engineer
    ↓
tio / minicom / picocom
    ↓
login manually
    ↓
leave authenticated shell running
    ↓
disconnect terminal program / release tty
    ↓
lmux zeromux probe ...
```

Zeromux must never:

* guess usernames;
* send passwords;
* store credentials;
* respond automatically to `login:` or `Password:` prompts;
* assume that opening a UART provides an authenticated shell.

Before probing target capabilities, Zeromux must establish shell control using a randomized sentinel exchange.

A known shell prompt such as `$`, `#` or `>` is not required and must not be used as the primary synchronization mechanism.

Command echo, arbitrary prompts, CR/LF variants and unrelated asynchronous console output must be tolerated.

Conceptually:

```text
host sends:
    <shell command containing random nonce>

target output contains:
    ZMX_BEGIN_<nonce>
    ...
    ZMX_END_<nonce>_<status>
```

Only output bounded by the matching nonce belongs to that transaction.

Unrelated bytes outside the markers are console noise and must not be interpreted as probe results.

If Zeromux observes a recognizable authentication prompt, or cannot establish sentinel-based shell command execution within the configured deadline, it must stop without target mutation and report a distinct result such as:

```text
AUTH_REQUIRED
NO_USABLE_SHELL
TIMEOUT
```

Absence of a recognizable login prompt does not by itself prove authentication. Successful shell sentinel execution does.

Authentication support, if ever added, is a separate future policy/provider layer and must not become implicit behavior of the bootstrap session.
