# LMUX Hands-On Quick Start

This page walks through the first successful LMUX session on a Linux host and a
USB-serial target. By the end you will have:

```text
host
  -> shell on target
  -> copy a file
  -> run a raw byte-stream command
  -> unplug/replug USB
  -> watch the same target execution continue
```

The concrete paths below match the tested Hantek DSO2D15-style baseline:
host carrier `/dev/ttyACM0`, target carrier `/dev/ttyGS0`, and target install
directory `/root/lmux/`. Those paths are examples, not fundamental LMUX
limits.

## Prerequisites

- Host tools are already built under `build/host/`.
- The target `lmuxd` binary is already installed on the target under
  `/root/lmux/`.
- The `lmux-start-target.sh` helper is copied next to target `lmuxd`, or you
  can use the equivalent command shown below.
- The target is connected to the host through the USB serial carrier.
- Host commands that reference `build/host/...` are run from the repository
  root.

For build commands, see the [README Build section](../README.md#build).

## Start The Target

On the target:

```sh
cd /root/lmux
./lmux-start-target.sh
```

The helper waits for `/dev/ttyGS0` and then execs the supervisor in OUTER mode.
It is equivalent to starting:

```sh
/root/lmux/lmuxd --supervisor --outer --carrier /dev/ttyGS0
```

Leave this running.

## Start The Host

On the host, from the repository root:

```sh
./start-host.sh
```

The script starts host `lmuxd` in the background. That daemon owns the host side
of the LMUX/OUTER connection and creates the local control and service sockets
under `/run/lmux/`.

## Check Status

```sh
build/host/lmux status
```

You do not need to read every counter. For a healthy first run, look for:

- `carrier: open`;
- `outer: ESTABLISHED`;
- `connection: ESTABLISHED`;
- desired services such as `console1`, `console2`, `files`, and `__exec` with
  `acknowledged=yes`;
- service listeners such as `console1` and `files` with `listener=ready`.

If those are present, the host and target have converged.

## Get A Terminal

Attach to the first target console:

```sh
build/host/lmux term 1
```

Try a few harmless commands:

```sh
pwd
uname -a
```

Detach from the terminal with:

```text
Ctrl-T q
```

Detach closes the transient LMUX stream, but the shell process stays alive in
the durable PTY service while the worker stays alive. Re-running
`build/host/lmux term 1` attaches to the same console service again.

On attach, LMUX does not replay detached output or reconstruct terminal state.
For the target's normal BusyBox ash, LMUX recognizes the idle line-editor tty
mode and asks the PTY to deliver `SIGINT` to that shell process group. Ash then
discards any unfinished input and prints its real current prompt; depending on
the shell, its normal `^C` interrupt marker may precede the prompt. Pressing
Enter after attach cannot execute the discarded text.

The reset is deliberately skipped when the foreground process group is not the
durable shell or the tty mode does not exactly match the known idle editor
mode. Thus attaching does not interrupt a running command, builtin, or raw-mode
application. In those cases LMUX waits for new live output and does not invent
a prompt. PTY input/output otherwise remains an uninterpreted byte stream.

## Copy A File

Create a small host file:

```sh
printf 'hello from LMUX\n' > /tmp/lmux-hello.txt
```

Upload it to the target:

```sh
build/host/lmux put /tmp/lmux-hello.txt /tmp/lmux-hello.txt
```

Download it back:

```sh
build/host/lmux get /tmp/lmux-hello.txt /tmp/lmux-hello.back
```

Compare the files on the host:

```sh
cmp /tmp/lmux-hello.txt /tmp/lmux-hello.back
```

`put` and `get` use the FILE service with size/hash verification; uploads use a
temporary file and atomic target-side replacement.

## Run A Foreground Byte Stream

Run one foreground target process with raw stdin/stdout/stderr bytes:

```sh
printf 'hello through LMUX\n' |
    build/host/lmux stream -- /bin/cat
```

Expected output:

```text
hello through LMUX
```

The three common data paths are:

```text
term    -> PTY / interactive terminal
put/get -> verified files
stream  -> raw stdin/stdout/stderr bytes of one foreground process
```

`stream` is not a PTY. The target program path must be absolute. For example,
use `/bin/cat`, not `cat`. Use `/bin/sh -c ...` only when you explicitly want
target shell behavior.

## Survive A Physical USB Reconnect

Start a long-running foreground target execution:

```sh
build/host/lmux stream -- /bin/sh -c \
    'i=0; while [ "$i" -lt 1000 ]; do echo "$i"; i=$((i+1)); sleep 1; done'
```

While the numbers are printing, physically unplug the USB cable or carrier and
plug it back in. Do not restart host `lmuxd`, the target supervisor, or the
target worker by hand.

You should see output pause while USB is gone, then continue after the host and
target converge again. The exact numbers will differ:

```text
26
27
[USB disconnected]
[USB connected again]
28
29
30
```

PASS: the same target execution continues after fresh OUTER/LMUX convergence,
as long as the target worker survived the carrier loss. If the worker is
replaced, private executions are lost and LMUX will report the foreground
execution as lost rather than pretending it continued.

Stop the example with:

```text
Ctrl-C
```

For a foreground stream, Ctrl-C asks LMUX to signal and clean up the target
execution.

## What Happened

The physical carrier was allowed to fail without pretending the old byte stream
was still intact:

```text
physical USB loss
    -> old OUTER session retired
    -> old LMUX session retired

USB returns
    -> fresh OUTER
    -> fresh LMUX
    -> services reconcile
    -> same-worker execution reattaches
```

The key idea is:

```text
carrier lifetime != LMUX session lifetime != service/execution lifetime
```

For the deeper model, start with [README Architecture](../README.md#architecture),
then the [OUTER/1 wire spec](outer1-wire.md) and
[LMUX/2 wire spec](lmux2-wire.md).

## Where Next

- Managed deployment -> [README Managed Deployment](../README.md#managed-deployment)
- MCP / debugging -> [docs/lmux-mcp.md](lmux-mcp.md),
  [docs/debugging.md](debugging.md)
- 1.0 supported contract -> [docs/release-1.0.md](release-1.0.md)
- Wire protocols -> [docs/outer1-wire.md](outer1-wire.md),
  [docs/lmux2-wire.md](lmux2-wire.md)
- Zeromux/bootstrap -> [docs/zeromux.md](zeromux.md)
