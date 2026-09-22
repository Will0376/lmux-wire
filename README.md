# LMUX Wire

LMUX Wire is a lightweight userspace multiplexer for embedded Linux. The
foreground daemon is `lmuxd`, the CLI client is `lmux`, and the runtime wire
transport is LMUX/2.

LMUX/2 carries services and transient byte streams over one full-duplex carrier.
The transport knows about factories, services, streams, bytes, flow control,
framing, and lifecycle. Application protocols such as LXF remain opaque bytes on
top of streams.

LMUX/1 was the historical fixed-channel predecessor and remains useful only as a
git-history reference point.

## Runtime Model

The target daemon advertises bounded built-in factories:

- `pty`
- `unix-rendezvous`
- `file`

The host daemon owns desired topology and ensures durable services by name:

- `console1`: `pty`, `/bin/sh`, supervised
- `console2`: `pty`, `/bin/sh`, supervised
- `gdb`: `unix-rendezvous`, target socket `/tmp/lmux-gdb.sock`
- `files`: `file`

Host-side sockets for the default deployment are:

- `/run/lmux/console1.sock`
- `/run/lmux/console2.sock`
- `/run/lmux/gdb.sock`
- `/run/lmux/file.sock`
- `/run/lmux/control.sock`

Service names are durable identities inside one target worker lifetime.
`service_id` values are runtime handles in that worker lifetime. `stream_id`
values are transient and connection-local; a physical reconnect creates a fresh
stream-id namespace and old streams cannot be reused.

## Programs

- `lmuxd`: foreground daemon owning the physical carrier.
- `lmux term [-v|--verbose] [--remote-dir DIR] [--channel 1|2] [1|2|SOCKET]`:
  raw terminal client with local Ctrl-T prefix controls.
- `lmux status [--control SOCKET]`: query the local host daemon status socket.
- `lmux connect SOCKET`: binary-clean stdin/stdout socket bridge.
- `lmux exec SOCKET -- PROGRAM [ARGS...]`: dup2s a socket to child stdin/stdout
  and execs directly.
- `lmux put [OPTIONS] HOST_SRC [TARGET_DST]`: upload, HOST to TARGET.
- `lmux get [OPTIONS] TARGET_SRC [HOST_DST]`: download, TARGET to HOST.

## Target Startup

The target daemon is zero-config with respect to service topology. It owns the
carrier and advertises factories; the host decides which services should exist.

```sh
/root/lmux/lmuxd --role target --carrier /dev/ttyGS0
```

The included target wrapper waits for `/dev/ttyGS0` and execs foreground
`lmuxd`:

```sh
/root/lmux/lmux-start
```

Intended BusyBox init entry:

```text
::respawn:/root/lmux/lmux-start
```

No separate getty or console scripts are needed for normal deployment.

## Host Startup

```sh
mkdir -p /run/lmux
/mnt/linfiles/SourceCode/lmux/build/host/lmuxd \
  --role host \
  --carrier /dev/ttyACM0 \
  --control /run/lmux/control.sock \
  --pty console1:/run/lmux/console1.sock:/bin/sh \
  --pty console2:/run/lmux/console2.sock:/bin/sh \
  --unix gdb:/run/lmux/gdb.sock:/tmp/lmux-gdb.sock \
  --file files:/run/lmux/file.sock
```

`start-host.sh` starts this topology and cleans stale owned sockets.

## Terminal Prefix

`lmux term` puts the local tty in raw mode. Ordinary bytes, including Ctrl-C and
Ctrl-Z, go to the remote PTY. Local control uses Ctrl-T:

```text
Ctrl-T ?       help
Ctrl-T q/.     disconnect
Ctrl-T Ctrl-T  send literal Ctrl-T
Ctrl-T p       upload file   HOST -> TARGET
Ctrl-T g       download file TARGET -> HOST
```

Ctrl-T `p` and `g` temporarily restore the HOST terminal to canonical mode,
prompt for paths, run the same LXF file service used by `lmux put/get`, then
restore raw mode and resume the same console socket. The console stream is not
closed.

`lmux term` and `lmux term 1` use console context 1. `lmux term 2` uses console
context 2. For a custom socket, add `--channel 1` or `--channel 2` if Ctrl-T file
operations should query that TARGET shell's current working directory:

```sh
lmux term --channel 2 /custom/console.sock
```

Ctrl-T PUT/GET display the HOST current working directory and the TARGET base
directory. The TARGET base comes from `--remote-dir DIR`, then absolute
`LMUX_REMOTE_DIR`, then the supervised TARGET shell cwd queried through the file
service, then `/root`.

`lmux term` sends the initial terminal size and later SIGWINCH changes to the
host daemon control socket. The host forwards resize control to the matching
PTY service, and the target applies it with `TIOCSWINSZ`; resize metadata is not
injected into the shell byte stream.

## Status

Host `lmuxd` can expose a local-only control socket, normally
`/run/lmux/control.sock` when started by `start-host.sh`.

```sh
lmux status
```

Example output:

```text
LMUX Wire
local:  0.1.0+build.N
wire:   LMUX/2
role:   host
link:   established
control: connected
services:
  console1 pty id=...
  console2 pty id=...
  gdb unix-rendezvous id=...
  files file id=...
```

## GDB

Target:

```sh
/root/lmux/lmux exec /tmp/lmux-gdb.sock -- gdbserver --attach stdio <PID>
```

Host GDB:

```text
target remote /run/lmux/gdb.sock
```

The `gdb` service is a generic Unix rendezvous service. There is no GDB-specific
daemon logic. GDB's raw all-stop interrupt byte, `0x03`, is transported as
ordinary stream data.

## File Transfer

Host file socket default:

```text
/run/lmux/file.sock
```

Override with `-S SOCKET` or `--socket SOCKET`.

HOST paths refer to the machine running `lmux`. TARGET paths refer to the
filesystem served by target `lmuxd`. Relative HOST paths resolve against the
HOST process cwd. Relative TARGET paths resolve against the TARGET base
directory: `--remote-dir`, absolute `LMUX_REMOTE_DIR`, queried TARGET shell cwd
for `--channel`, or `/root`.

Upload, HOST to TARGET:

```sh
lmux put ./lmuxd
lmux put ./lmuxd /root/lmux/lmuxd
lmux put --mode 0755 ./lmuxd /root/lmux/lmuxd.new
lmux put - /tmp/stdin.bin
```

Download, TARGET to HOST:

```sh
lmux get log.log
lmux get /root/log.log ./saved.log
lmux get /tmp/output.bin -
lmux get -S /custom/file.sock /tmp/foo ./foo
```

`--channel 1` or `--channel 2` selects which supervised TARGET shell cwd is
queried for relative TARGET paths. Explicit absolute paths remain unchanged.
Directory recursion and automatic parent-directory creation are not supported.

When stderr is a TTY, transfers show a compact progress line. Use `-v` or
`--verbose` with `lmux put`, `lmux get`, or `lmux term` for detailed progress.
Progress is always written to stderr. `lmux get TARGET -` keeps downloaded bytes
on stdout only.

The file service uses the LXF/1 application protocol over the `files` service:

```text
magic "LXF1"
u8 type: 1 PUT_REQUEST, 2 GET_REQUEST, 3 OK, 4 ERROR, 5 DATA, 6 DONE, 7 CWD_REQUEST, 8 CWD_REPLY
u8 flags
u16 reserved = 0
u32 payload_length LE
payload
```

PUT streams into a temporary regular file in the destination directory using
`O_CREAT|O_EXCL`, verifies byte count and SHA-256, applies the requested mode,
fsyncs, and atomically renames over the final path. Existing regular files may
be replaced; symlinks, directories, devices, FIFOs, and sockets are rejected
using final-component `lstat()` checks.

GET requires a TARGET regular file, streams DATA records, and sends DONE with
byte count and SHA-256. HOST `lmux get` writes to a same-directory temp file and
atomically renames on successful verification. For `HOST_DST = -`, data goes
directly to stdout and all diagnostics stay on stderr.

CWD support currently belongs to the LXF/frontend path: `LXF_T_CWD_REQUEST`
maps console context 1 or 2 to PTY services `console1` or `console2` and reads
`/proc/<shell-pid>/cwd` on the target. It does not parse shell prompts.

## Build

```sh
make clean
make host
make selftest
make target
make strip-target
```

Target builds use `arm-linux-gnueabi-gcc`, ARMv5TE, soft-float, and static
linking.

## Version

```sh
lmux --version
lmuxd --version
```

Version strings use `BASE+build.N`, for example `0.1.0+build.104`. `BASE` comes
from `VERSION`; `N` is a persistent monotonic build number shared by `lmux`,
`lmuxd`, and the host/target binaries from the same build cycle.

Human-facing version output includes the project name, for example
`lmux 0.1.0+build.104 (LMUX Wire)`.

## Reconnect

Physical carrier reconnect is distinct from service lifetime. When a physical
LMUX/2 connection is lost, all transient streams become terminal and stream IDs
restart in the new connection namespace. Durable target services may survive
inside the same daemon process. Host startup/reconnect performs discovery,
receives the service snapshot, reconciles desired services by name, and opens
new transient streams.

PTY services keep the supervised `/bin/sh` process across stream detach and
carrier reconnect while the target daemon remains alive. File transfers are
transient: an active LXF transfer aborts on physical disconnect and the durable
`files` service remains usable after reconnect.

## Limits

- `LMUX2_MAX_STREAMS = 16`.
- Per-stream RX/TX rings are 1024 bytes.
- Connection receive window is 4096 bytes.
- Control/lifecycle queue capacity is 32 frames.
- Maximum LMUX/2 frame payload is 512 bytes.
- Service registry capacity is 8 services.
- Host desired service capacity is 8 services.
- Per-service binding capacity is 8 bindings.
- Runtime poll table capacity is 64 fds.
- LXF DATA payloads are bounded to 4096 bytes and are streamed through normal
  LMUX/2 flow control.
- Internal paths are bounded to 255 bytes.
- No authentication, encryption, compression, TCP, retransmission, directory
  recursion, or remote command protocol.
