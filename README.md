# LMUX Wire

LMUX Wire is a lightweight userspace multiplexer for carrying multiple logical byte streams over one serial-like transport. The CLI client is `lmux`, the foreground daemon is `lmuxd`, the transport protocol is LMUX/1, and the channel-4 file-transfer protocol is LXF/1.

Current features include supervised interactive PTY consoles, raw stream channels for uses such as GDB, built-in file transfer with HOST/TARGET path semantics, Ctrl-T terminal controls, TARGET shell cwd awareness, and transfer progress reporting.

## Channel Layout

- channel 1: interactive console 1
- channel 2: interactive console 2
- channel 3: generic raw stream, intended for GDB
- channel 4: file-transfer stream

LMUX/1 still uses COBS framing, CRC-16/CCITT-FALSE, HELLO/OPEN/CLOSE/DATA/CREDIT/RESET, fixed 1024-byte per-channel RX/TX rings, and round-robin DATA scheduling. `LMUX_MAX_PAYLOAD` remains 256 bytes.

## Programs

- `lmuxd`: foreground daemon owning the physical carrier.
- `lmux term [-v|--verbose] [--remote-dir DIR] [--channel 1|2] [1|2|SOCKET]`: raw terminal client with local Ctrl-T prefix controls. With no argument it uses `/run/lmux/console1.sock`; `1` and `2` select the default console sockets.
- `lmux connect SOCKET`: binary-clean stdin/stdout socket bridge.
- `lmux exec SOCKET -- PROGRAM [ARGS...]`: dup2s a socket to child stdin/stdout and execs directly.
- `lmux put [OPTIONS] HOST_SRC [TARGET_DST]`: upload, HOST -> TARGET.
- `lmux get [OPTIONS] TARGET_SRC [HOST_DST]`: download, TARGET -> HOST.

## Target Startup

Normal DSO target startup is one supervised `lmuxd`:

```sh
/root/lmux/lmuxd \
  --carrier /dev/ttyGS0 \
  --shell 1:/dev/lmux1 \
  --shell 2:/dev/lmux2 \
  --socket 3:/tmp/lmux-gdb.sock \
  --file 4
```

`--shell` creates a Unix98 PTY, creates the stable symlink, spawns `/bin/sh` on the slave side, reaps it, and respawns it after about one second if it exits. The shell belongs to the local `lmuxd` lifetime, so host detach, USB reconnect, or remote CLOSE do not kill the console session. On daemon shutdown, managed shells receive SIGHUP and are reaped with a bounded SIGKILL fallback.

The included target wrapper waits for `/dev/ttyGS0` and execs foreground `lmuxd`:

```sh
/root/lmux/lmux-start
```

Intended BusyBox init entry:

```text
::respawn:/root/lmux/lmux-start
```

No separate getty or console scripts are needed for normal deployment. `--pty` remains available as a generic PTY endpoint.

## Host Startup

```sh
mkdir -p /run/lmux
/mnt/linfiles/SourceCode/lmux/build/host/lmuxd \
  --carrier /dev/ttyACM0 \
  --socket 1:/run/lmux/console1.sock \
  --socket 2:/run/lmux/console2.sock \
  --socket 3:/run/lmux/gdb.sock \
  --socket 4:/run/lmux/file.sock
```

`start-host.sh` starts this layout and cleans stale owned sockets including `file.sock`.

## Terminal Prefix

`lmux term` puts the local tty in raw mode. Ordinary bytes, including Ctrl-C and Ctrl-Z, go to the remote PTY. Local control uses Ctrl-T:

```text
Ctrl-T ?       help
Ctrl-T q/.     disconnect
Ctrl-T Ctrl-T  send literal Ctrl-T
Ctrl-T p       upload file   HOST -> TARGET
Ctrl-T g       download file TARGET -> HOST
```

Unknown Ctrl-T combinations preserve input by sending literal Ctrl-T and the following byte.

Ctrl-T `p` and `g` temporarily restore the HOST terminal to canonical mode, prompt for paths, run the same channel-4 PUT/GET engine used by `lmux put/get`, then restore raw mode and resume the same console socket. The console socket is not closed. During the modal transfer, console output is naturally backpressured by its LMUX channel while file data uses channel 4.

`lmux term` and `lmux term 1` use console context 1. `lmux term 2` uses console context 2. For a custom socket, add `--channel 1` or `--channel 2` if Ctrl-T file operations should query that TARGET shell's current working directory:

```sh
lmux term --channel 2 /custom/console.sock
```

Ctrl-T PUT/GET display the HOST current working directory and the TARGET base directory. The TARGET base comes from `--remote-dir DIR`, then absolute `LMUX_REMOTE_DIR`, then the supervised TARGET shell cwd queried through channel 4, then `/root`.

## GDB

Target:

```sh
/root/lmux/lmux exec /tmp/lmux-gdb.sock -- gdbserver --attach stdio <PID>
```

Host GDB:

```text
target remote /run/lmux/gdb.sock
```

Channel 3 remains a generic raw stream; there is no GDB-specific daemon logic.

## File Transfer

Host file socket default:

```text
/run/lmux/file.sock
```

Override with `-S SOCKET` or `--socket SOCKET`.

HOST paths refer to the machine running `lmux`. TARGET paths refer to the filesystem served by target `lmuxd`. Relative HOST paths resolve against the HOST process cwd. Relative TARGET paths resolve against the TARGET base directory: `--remote-dir`, absolute `LMUX_REMOTE_DIR`, queried TARGET shell cwd for `--channel`, or `/root`.

Upload, HOST -> TARGET:

```sh
lmux put ./lmuxd
lmux put ./lmuxd /root/lmux/lmuxd
lmux put --mode 0755 ./lmuxd /root/lmux/lmuxd.new
lmux put - /tmp/stdin.bin
```

Download, TARGET -> HOST:

```sh
lmux get log.log
lmux get /root/log.log ./saved.log
lmux get /tmp/output.bin -
lmux get -S /custom/file.sock /tmp/foo ./foo
```

`--channel 1` or `--channel 2` selects which supervised TARGET shell cwd is queried for relative TARGET paths. Explicit absolute paths remain unchanged. Directory recursion and automatic parent-directory creation are not supported.

When stderr is a TTY, transfers show a compact progress line such as:

```text
PUT [############------------] 50%
GET [##################------] 75%
```

Use `-v` or `--verbose` with `lmux put`, `lmux get`, or `lmux term` for detailed Ctrl-T transfer progress:

```text
PUT [############------] 67%  344.8 KiB/512.0 KiB  2.37 MiB/s  elapsed 12s  ETA 6s
```

Progress is always written to stderr. Non-TTY stderr does not receive carriage-return animation, preserving script and MCP-friendly logs. `lmux get TARGET -` keeps downloaded bytes on stdout only.

The file service uses the LXF/1 application protocol on channel 4:

```text
magic "LXF1"
u8 type: 1 PUT_REQUEST, 2 GET_REQUEST, 3 OK, 4 ERROR, 5 DATA, 6 DONE, 7 CWD_REQUEST, 8 CWD_REPLY
u8 flags
u16 reserved = 0
u32 payload_length LE
payload
```

PUT streams into a temporary regular file in the destination directory using `O_CREAT|O_EXCL`, verifies byte count and SHA-256, applies the requested mode, fsyncs, and atomically renames over the final path. Existing regular files may be replaced; symlinks, directories, devices, FIFOs, and sockets are rejected using final-component `lstat()` checks.

GET requires a TARGET regular file, streams DATA records, and sends DONE with byte count and SHA-256. HOST `lmux get` writes to a same-directory temp file and atomically renames on successful verification. For `HOST_DST = -`, data goes directly to stdout and all diagnostics stay on stderr.

## Build

```sh
make clean
make host
make selftest
make target
make strip-target
```

Target builds use `arm-linux-gnueabi-gcc`, ARMv5TE, soft-float, and static linking.

## Version

```sh
lmux --version
lmuxd --version
```

Version strings use `BASE+build.N`, for example `0.1.0+build.37`. `BASE` comes from `VERSION`; `N` is a persistent monotonic build number shared by `lmux`, `lmuxd`, and the host/target binaries from the same build cycle.

Human-facing version output includes the project name, for example `lmux 0.1.0+build.37 (LMUX Wire)`.

## Reconnect And Reset

On carrier open, `lmuxd` applies raw termios, flushes tty queues, generates a new local session, and starts HELLO. Before HELLO, stale malformed frames and stale OPEN/CREDIT/DATA are discarded without RESET. After establishment, protocol faults remain strict. HELLO responses are ordered before OPEN advertisements so one peer losing the first HELLO cannot deadlock startup.

## Limits

- Exactly four fixed LMUX/1 channels.
- `LMUX_MAX_PAYLOAD = 256`.
- Per-channel LMUX RX/TX rings are 1024 bytes.
- LXF DATA payloads are bounded to 4096 bytes and are streamed through the normal LMUX scheduler.
- Internal paths are bounded to 255 bytes.
- No authentication, encryption, compression, TCP, retransmission, directory recursion, or remote command protocol.
