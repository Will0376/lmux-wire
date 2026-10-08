package gdbmi

import (
	"bufio"
	"context"
	"fmt"
	"os"
	"strings"
	"testing"
	"time"
)

func TestMain(m *testing.M) {
	if os.Getenv("LMUX_GDBMI_FAKE_HELPER") == "1" {
		fakeGDB()
		return
	}
	os.Exit(m.Run())
}

func fakeGDB() {
	if os.Getenv("LMUX_GDBMI_FAKE_EXIT_IMMEDIATE") == "1" {
		fmt.Fprintln(os.Stderr, "FAKE_GDBMI_EXITED")
		return
	}
	fmt.Println(`=thread-created,id="1",group-id="i1"`)
	fmt.Println(`(gdb) `)
	sc := bufio.NewScanner(os.Stdin)
	for sc.Scan() {
		line := sc.Text()
		i := 0
		for i < len(line) && line[i] >= '0' && line[i] <= '9' {
			i++
		}
		tok, cmd := line[:i], line[i:]
		switch {
		case strings.HasPrefix(cmd, "-file-exec-and-symbols"):
			fmt.Printf("%s^done\n", tok)
			fmt.Println(`(gdb)`)
		case strings.HasPrefix(cmd, "-target-select"):
			fmt.Printf("%s^connected\n", tok)
			fmt.Println(`(gdb) `)
			fmt.Println(`*stopped,reason="signal-received",thread-id="1",frame={level="0",addr="0x1000",func="_start"}`)
		case strings.HasPrefix(cmd, "-break-insert"):
			fmt.Printf(`%s^done,bkpt={number="1",addr="0x1010",func="foo",file="foo.c",line="7"}`+"\n", tok)
			fmt.Println(`(gdb)`)
		case strings.HasPrefix(cmd, "-exec-continue"):
			fmt.Printf("%s^running\n", tok)
			fmt.Println(`(gdb)`)
			fmt.Println(`*running,thread-id="all"`)
			fmt.Println(`*stopped,reason="breakpoint-hit",thread-id="1",frame={level="0",addr="0x1010",func="foo",file="foo.c",fullname="/tmp/foo.c",line="7"}`)
		case strings.HasPrefix(cmd, "-stack-list-frames"):
			fmt.Printf(`%s^done,stack=[frame={level="0",addr="0x1010",func="foo",file="foo.c",fullname="/tmp/foo.c",line="7"}]`+"\n", tok)
			fmt.Println(`(gdb)`)
		case strings.HasPrefix(cmd, "-data-evaluate-expression"):
			fmt.Printf(`%s^done,value="42"`+"\n", tok)
			fmt.Println(`(gdb)`)
		case strings.HasPrefix(cmd, "-gdb-exit"), strings.HasPrefix(cmd, "-target-detach"):
			fmt.Printf("%s^done\n", tok)
			fmt.Println(`(gdb)`)
			return
		default:
			fmt.Printf(`%s^error,msg="unexpected command"`+"\n", tok)
			fmt.Println(`(gdb)`)
		}
	}
}

func TestProcessCommandsWithFakeGDB(t *testing.T) {
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	p, err := startFakeProcess(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer p.Close()
	if rec, err := p.Command(ctx, "-file-exec-and-symbols /tmp/a.out"); err != nil || rec.Class != "done" {
		t.Fatalf("symbols rec=%#v err=%v", rec, err)
	}
	if rec, err := p.Command(ctx, "-file-exec-and-symbols /tmp/b.out"); err != nil || rec.Class != "done" || rec.Token != 2 {
		t.Fatalf("second symbols rec=%#v err=%v", rec, err)
	}
	if rec, err := p.Command(ctx, "-target-select remote /tmp/sock"); err != nil || rec.Class != "connected" {
		t.Fatalf("target rec=%#v err=%v", rec, err)
	}
	if got, want := string(p.LastTargetSelectBytes()), "3-target-select remote /tmp/sock\n"; got != want {
		t.Fatalf("last target-select bytes = %q want %q", got, want)
	}
	waitEventClass(t, p, "stopped")
	if rec, err := p.Command(ctx, "-break-insert foo"); err != nil || rec.Class != "done" {
		t.Fatalf("break rec=%#v err=%v", rec, err)
	}
	if rec, err := p.Command(ctx, "-exec-continue"); err != nil || rec.Class != "running" {
		t.Fatalf("continue rec=%#v err=%v", rec, err)
	}
	waitEventClass(t, p, "running")
	waitEventClass(t, p, "stopped")
	if rec, err := p.Command(ctx, "-stack-list-frames 0 4"); err != nil || rec.Class != "done" {
		t.Fatalf("bt rec=%#v err=%v", rec, err)
	}
	if rec, err := p.Command(ctx, "-data-evaluate-expression x"); err != nil || rec.Class != "done" {
		t.Fatalf("eval rec=%#v err=%v", rec, err)
	}
}

func TestProcessErrorProvenance(t *testing.T) {
	p := &Process{stdin: errWriteCloser{err: os.ErrClosed}, pending: map[int]chan Record{}, events: make(chan Event, 1)}
	if _, err := p.Command(context.Background(), "-file-exec-and-symbols /tmp/a.out"); err == nil || !strings.Contains(err.Error(), "GDB_MI_WRITE_FAILED") {
		t.Fatalf("expected GDB_MI_WRITE_FAILED, got %v", err)
	}

	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	env := append(os.Environ(), "LMUX_GDBMI_FAKE_HELPER=1", "LMUX_GDBMI_FAKE_EXIT_IMMEDIATE=1")
	exited, err := StartEnv(ctx, env, os.Args[0], "-test.run=TestProcessErrorProvenance")
	if err != nil {
		t.Fatal(err)
	}
	if _, err := exited.Command(ctx, "-file-exec-and-symbols /tmp/a.out"); err == nil ||
		(!strings.Contains(err.Error(), "GDB_EXITED") && !strings.Contains(err.Error(), "GDB_MI_WRITE_FAILED")) {
		t.Fatalf("expected GDB exit/write provenance, got %v", err)
	}
}

type errWriteCloser struct {
	err error
}

func (e errWriteCloser) Write(p []byte) (int, error) { return 0, e.err }
func (e errWriteCloser) Close() error                { return nil }

func startFakeProcess(ctx context.Context) (*Process, error) {
	env := append(os.Environ(), "LMUX_GDBMI_FAKE_HELPER=1")
	return StartEnv(ctx, env, os.Args[0], "-test.run=TestProcessCommandsWithFakeGDB")
}

func waitEventClass(t *testing.T, p *Process, class string) Record {
	t.Helper()
	timer := time.NewTimer(time.Second)
	defer timer.Stop()
	for {
		select {
		case ev, ok := <-p.Events():
			if !ok {
				t.Fatalf("events closed")
			}
			if ev.Record.Class == class {
				return ev.Record
			}
		case <-timer.C:
			t.Fatalf("timeout waiting for %s", class)
		}
	}
}
