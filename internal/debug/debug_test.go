package debug

import (
	"bufio"
	"bytes"
	"context"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"net"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"testing"
	"time"

	"lmux/internal/gdbmi"
	"lmux/internal/hc2"
)

func TestMain(m *testing.M) {
	if os.Getenv("LMUX_DEBUG_FAKE_GDB") == "1" {
		fakeDebugGDB()
		return
	}
	os.Exit(m.Run())
}

func fakeDebugGDB() {
	if os.Getenv("LMUX_DEBUG_FAKE_GDB_EXIT_IMMEDIATE") == "1" {
		fmt.Fprintln(os.Stderr, "HOST_GDB_DIED")
		return
	}
	sc := bufio.NewScanner(os.Stdin)
	var rsp net.Conn
	fmt.Println(`=thread-group-added,id="i1"`)
	fmt.Println(`(gdb) `)
	for sc.Scan() {
		line := sc.Text()
		logFakeGDBCommand(line)
		i := 0
		for i < len(line) && line[i] >= '0' && line[i] <= '9' {
			i++
		}
		tok, cmd := line[:i], line[i:]
		switch {
		case strings.HasPrefix(cmd, "-file-exec-and-symbols"):
			fmt.Printf("%s^done\n", tok)
			fmt.Println(`(gdb)`)
		case strings.HasPrefix(cmd, "-interpreter-exec console"):
			fmt.Printf("%s^done\n", tok)
			fmt.Println(`(gdb)`)
		case strings.HasPrefix(cmd, "-gdb-set"):
			fmt.Printf("%s^done\n", tok)
			fmt.Println(`(gdb)`)
		case strings.HasPrefix(cmd, "-target-select"):
			if os.Getenv("LMUX_DEBUG_FAKE_GDB_TARGET_ERROR") == "1" {
				fmt.Printf(`%s^error,msg="remote unavailable"`+"\n", tok)
				continue
			}
			parts := strings.Split(cmd, " ")
			path := strings.Trim(parts[len(parts)-1], `"`)
			if os.Getenv("LMUX_DEBUG_FAKE_GDB_UNLINK_TARGET_SOCKET") == "1" {
				_ = os.Remove(path)
				fmt.Printf(`%s^error,msg="remote unavailable"`+"\n", tok)
				continue
			}
			if st, err := os.Lstat(path); err != nil || st.Mode()&os.ModeSocket == 0 {
				fmt.Printf(`%s^error,msg="RSP_SOCKET_MISSING"`+"\n", tok)
				continue
			}
			if c, err := net.Dial("unix", path); err == nil {
				rsp = c
				if n, _ := strconv.Atoi(os.Getenv("LMUX_DEBUG_FAKE_GDB_RSP_BYTES")); n > 0 {
					payload := make([]byte, n)
					for i := range payload {
						payload[i] = byte(i * 31)
					}
					_, _ = rsp.Write(payload)
				} else {
					_, _ = rsp.Write([]byte("gdb-to-target"))
				}
				if os.Getenv("LMUX_DEBUG_FAKE_GDB_RSP_CLOSE") == "1" {
					_ = rsp.Close()
				}
				if os.Getenv("LMUX_DEBUG_FAKE_GDB_RSP_CLOSE_THEN_ERROR") == "1" {
					_ = rsp.Close()
					fmt.Printf(`%s^error,msg="remote closed"`+"\n", tok)
					continue
				}
				if os.Getenv("LMUX_DEBUG_FAKE_GDB_RSP_CLOSE_THEN_EXIT") == "1" {
					_ = rsp.Close()
					fmt.Fprintln(os.Stderr, "HOST_GDB_EXIT_AFTER_RSP_CLOSE")
					return
				}
			} else {
				fmt.Printf(`%s^error,msg="RSP_DIAL_FAILED: %v"`+"\n", tok, err)
				continue
			}
			if delay := os.Getenv("LMUX_DEBUG_FAKE_GDB_TARGET_SELECT_DELAY_MS"); delay != "" {
				if d, err := time.ParseDuration(delay + "ms"); err == nil {
					time.Sleep(d)
				}
			}
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
		case strings.HasPrefix(cmd, "-exec-interrupt"):
			fmt.Printf("%s^done\n", tok)
			fmt.Println(`(gdb)`)
			if os.Getenv("LMUX_DEBUG_FAKE_GDB_INTERRUPT_STOPS_LATER") == "1" {
				go func() {
					time.Sleep(50 * time.Millisecond)
					fmt.Println(`*stopped,reason="signal-received",thread-id="1",frame={level="0",addr="0x1030",func="select"}`)
				}()
			} else {
				fmt.Println(`*stopped,reason="signal-received",thread-id="1",frame={level="0",addr="0x1030",func="select"}`)
			}
		case strings.HasPrefix(cmd, "-exec-step-instruction"), strings.HasPrefix(cmd, "-exec-next-instruction"):
			fmt.Printf("%s^running\n", tok)
			fmt.Println(`(gdb)`)
			fmt.Println(`*running,thread-id="all"`)
			fmt.Println(`*stopped,reason="end-stepping-range",thread-id="1",frame={level="0",addr="0x1024",func="bar",line="9"}`)
		case strings.HasPrefix(cmd, "-exec-step"):
			fmt.Printf("%s^running\n", tok)
			fmt.Println(`(gdb)`)
			fmt.Println(`*running,thread-id="all"`)
			if os.Getenv("LMUX_DEBUG_FAKE_GDB_STEP_STAYS_RUNNING") != "1" {
				fmt.Println(`*stopped,reason="end-stepping-range",thread-id="1",frame={level="0",addr="0x1020",func="bar",line="9"}`)
			}
		case strings.HasPrefix(cmd, "-exec-next"), strings.HasPrefix(cmd, "-exec-finish"):
			fmt.Printf("%s^running\n", tok)
			fmt.Println(`(gdb)`)
			fmt.Println(`*running,thread-id="all"`)
			fmt.Println(`*stopped,reason="end-stepping-range",thread-id="1",frame={level="0",addr="0x1020",func="bar",line="9"}`)
		case strings.HasPrefix(cmd, "-stack-list-frames"):
			fmt.Printf(`%s^done,stack=[frame={level="0",addr="0x1010",func="foo",file="foo.c",fullname="/tmp/foo.c",line="7"}]`+"\n", tok)
			fmt.Println(`(gdb)`)
		case strings.HasPrefix(cmd, "-data-evaluate-expression"):
			fmt.Printf(`%s^done,value="42"`+"\n", tok)
			fmt.Println(`(gdb)`)
		case strings.HasPrefix(cmd, "-target-detach"), strings.HasPrefix(cmd, "-gdb-exit"):
			fmt.Printf("%s^done\n", tok)
			fmt.Println(`(gdb)`)
			if strings.HasPrefix(cmd, "-gdb-exit") {
				if rsp != nil {
					_ = rsp.Close()
				}
				return
			}
		default:
			fmt.Printf(`%s^error,msg="unexpected command"`+"\n", tok)
		}
	}
}

func logFakeGDBCommand(line string) {
	path := os.Getenv("LMUX_DEBUG_FAKE_GDB_COMMAND_LOG")
	if path == "" {
		return
	}
	f, err := os.OpenFile(path, os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0600)
	if err != nil {
		return
	}
	defer f.Close()
	_, _ = f.WriteString(line + "\n")
}

type fakeHC2 struct {
	mu             sync.Mutex
	nextRef        uint64
	stdin          map[uint64][]byte
	signals        []byte
	releases       []uint64
	stdinCloses    []uint64
	actions        []string
	failStart      bool
	stdinErr       error
	stdinErrs      []error
	stdinPlan      func(int, uint64, []byte) error
	stdinRequests  int
	acceptedCh     chan []byte
	backpressureCh chan struct{}
	onStart        func(uint64)
	onStdinClose   func(uint64)
	onSignal       func(uint64, byte)
	startArgv      [][]string
}

func (f *fakeHC2) Request(ctx context.Context, typ uint16, payload []byte) (hc2.Frame, error) {
	f.mu.Lock()
	defer f.mu.Unlock()
	switch typ {
	case hc2.TypeExecStart:
		if f.failStart {
			return hc2.Frame{}, errors.New("start failed")
		}
		if _, argv, ok := decodeExecStartPayload(payload); ok {
			f.startArgv = append(f.startArgv, append([]string(nil), argv...))
		}
		f.nextRef++
		ref := f.nextRef
		if f.onStart != nil {
			go f.onStart(ref)
		}
		return hc2.Frame{Type: hc2.TypeExecStarted, Payload: execStatePayload(ref, 100+ref, 2)}, nil
	case hc2.TypeExecStdinData:
		ref := binary.LittleEndian.Uint64(payload[:8])
		data := append([]byte(nil), payload[8:]...)
		f.stdinRequests++
		req := f.stdinRequests
		var err error
		switch {
		case f.stdinErr != nil:
			err = f.stdinErr
		case len(f.stdinErrs) > 0:
			err = f.stdinErrs[0]
			f.stdinErrs = f.stdinErrs[1:]
		case f.stdinPlan != nil:
			err = f.stdinPlan(req, ref, data)
		}
		if err != nil {
			if isStdinBackpressure(err) && f.backpressureCh != nil {
				select {
				case f.backpressureCh <- struct{}{}:
				default:
				}
			}
			return hc2.Frame{}, err
		}
		if f.stdin == nil {
			f.stdin = map[uint64][]byte{}
		}
		f.stdin[ref] = append(f.stdin[ref], data...)
		if f.acceptedCh != nil {
			cp := append([]byte(nil), data...)
			select {
			case f.acceptedCh <- cp:
			default:
			}
		}
		return hc2.Frame{Type: hc2.TypeError, Payload: okPayload()}, nil
	case hc2.TypeExecStdinClose:
		if len(payload) >= 8 {
			ref := binary.LittleEndian.Uint64(payload[:8])
			f.stdinCloses = append(f.stdinCloses, ref)
			f.actions = append(f.actions, "stdin-close")
			if f.onStdinClose != nil {
				go f.onStdinClose(ref)
			}
		}
		return hc2.Frame{Type: hc2.TypeError, Payload: okPayload()}, nil
	case hc2.TypeExecSignal:
		if len(payload) >= 9 {
			ref := binary.LittleEndian.Uint64(payload[:8])
			sig := payload[8]
			f.signals = append(f.signals, sig)
			f.actions = append(f.actions, fmt.Sprintf("signal-%d", sig))
			if f.onSignal != nil {
				go f.onSignal(ref, sig)
			}
		}
		return hc2.Frame{Type: hc2.TypeError, Payload: okPayload()}, nil
	case hc2.TypeExecRelease:
		if len(payload) >= 8 {
			ref := binary.LittleEndian.Uint64(payload[:8])
			f.releases = append(f.releases, ref)
			f.actions = append(f.actions, "release")
		}
		return hc2.Frame{Type: hc2.TypeError, Payload: okPayload()}, nil
	default:
		return hc2.Frame{Type: hc2.TypeError, Payload: okPayload()}, nil
	}
}

func TestDebugLifecycleFakeGDB(t *testing.T) {
	t.Setenv("LMUX_DEBUG_FAKE_GDB", "1")
	dir := t.TempDir()
	sym := dir + "/a.out"
	if err := os.WriteFile(sym, []byte("symbols"), 0600); err != nil {
		t.Fatal(err)
	}
	fhc := &fakeHC2{}
	m := NewManager(Config{
		RuntimeDir:      dir,
		GDBPath:         os.Args[0],
		TargetGDBServer: "/target/gdbserver",
		MaxSessions:     2,
	}, fhc, func(string, map[string]any) {})
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	s, err := m.Start(ctx, StartRequest{Mode: "launch", Symbols: sym, Program: "/bin/app", Args: []string{"x"}, Label: "test"})
	if err != nil {
		t.Fatal(err)
	}
	waitSessionState(t, s, "STOPPED")
	if s.Token == "" || s.ExecRef == 0 {
		t.Fatalf("bad session token=%q ref=%d", s.Token, s.ExecRef)
	}
	assertSocketPath(t, s.socketPath)
	_ = waitForStdin(t, fhc, s.ExecRef, "gdb-to-target")
	fhc.mu.Lock()
	stdin := string(fhc.stdin[s.ExecRef])
	fhc.mu.Unlock()
	if !strings.Contains(stdin, "gdb-to-target") {
		t.Fatalf("bridge did not deliver GDB bytes to target stdin: %q", stdin)
	}
	bp, err := s.Breakpoint(ctx, "foo", false, "")
	if err != nil || bp["breakpoint_id"] != "1" {
		t.Fatalf("bp=%v err=%v", bp, err)
	}
	if _, err := s.Continue(ctx); err != nil {
		t.Fatal(err)
	}
	w := s.Wait(ctx, time.Second)
	if w["state"] != "STOPPED" || w["stop_reason"] != "breakpoint-hit" {
		t.Fatalf("bad wait: %#v", w)
	}
	bt, err := s.Backtrace(ctx, 8)
	if err != nil || len(bt["frames"].([]map[string]any)) != 1 {
		t.Fatalf("bt=%#v err=%v", bt, err)
	}
	if frames := bt["frames"].([]map[string]any); frames[0]["address"] != "0x1010" || frames[0]["function"] != "foo" || frames[0]["level"] != "0" {
		t.Fatalf("backtrace frame not decoded: %#v", frames)
	}
	ev, err := s.Evaluate(ctx, "x")
	if err != nil || ev["value"] != "42" {
		t.Fatalf("eval=%#v err=%v", ev, err)
	}
	if _, err := s.Detach(ctx); err != nil {
		t.Fatal(err)
	}
	if m.ActiveCount() != 0 {
		t.Fatalf("session leaked")
	}
	if _, err := os.Stat(s.socketPath); !os.IsNotExist(err) {
		t.Fatalf("socket leaked: %v", err)
	}
}

func TestDebugAsyncStepInterruptDoesNotWaitForStop(t *testing.T) {
	t.Setenv("LMUX_DEBUG_FAKE_GDB", "1")
	t.Setenv("LMUX_DEBUG_FAKE_GDB_STEP_STAYS_RUNNING", "1")
	t.Setenv("LMUX_DEBUG_FAKE_GDB_INTERRUPT_STOPS_LATER", "1")
	dir := t.TempDir()
	t.Setenv("LMUX_DEBUG_FAKE_GDB_COMMAND_LOG", filepath.Join(dir, "gdb.commands"))
	sym := writeSymbols(t, dir)
	m := NewManager(Config{
		RuntimeDir:      dir,
		GDBPath:         os.Args[0],
		TargetGDBServer: "/target/gdbserver",
		MaxSessions:     2,
	}, &fakeHC2{}, nil)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	s, err := m.Start(ctx, StartRequest{Mode: "launch", Symbols: sym, Program: "/bin/app"})
	if err != nil {
		t.Fatal(err)
	}
	waitSessionState(t, s, "STOPPED")
	step, err := s.Step(ctx, "step")
	if err != nil {
		t.Fatal(err)
	}
	if step["state"] != "RUNNING" {
		t.Fatalf("step should return after ^running with session RUNNING, got %#v", step)
	}
	intr, err := s.Interrupt(ctx)
	if err != nil {
		t.Fatal(err)
	}
	w := intr
	switch intr["state"] {
	case "RUNNING":
		w = s.Wait(ctx, time.Second)
	case "STOPPED":
		// The asynchronous stop may be published before Interrupt takes its
		// result snapshot. Interrupt does not require STOPPED, but it may
		// legitimately observe it.
	default:
		t.Fatalf("unexpected interrupt state: %#v", intr)
	}
	if w["state"] != "STOPPED" || w["stop_reason"] != "signal-received" {
		t.Fatalf("interrupt stop was not observed: %#v", w)
	}
	log := mustReadString(t, filepath.Join(dir, "gdb.commands"))
	asyncIdx := strings.Index(log, "-gdb-set mi-async on")
	targetIdx := strings.Index(log, "-target-select")
	if asyncIdx < 0 || targetIdx < 0 || asyncIdx > targetIdx {
		t.Fatalf("mi-async must be enabled before target-select, log=%q", log)
	}
	_, _ = s.Detach(ctx)
}

func TestDebugInstructionStepMapping(t *testing.T) {
	t.Setenv("LMUX_DEBUG_FAKE_GDB", "1")
	dir := t.TempDir()
	logPath := filepath.Join(dir, "gdb.commands")
	t.Setenv("LMUX_DEBUG_FAKE_GDB_COMMAND_LOG", logPath)
	sym := writeSymbols(t, dir)
	m := NewManager(Config{
		RuntimeDir:      dir,
		GDBPath:         os.Args[0],
		TargetGDBServer: "/target/gdbserver",
		MaxSessions:     2,
	}, &fakeHC2{}, nil)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	s, err := m.Start(ctx, StartRequest{Mode: "launch", Symbols: sym, Program: "/bin/app"})
	if err != nil {
		t.Fatal(err)
	}
	waitSessionState(t, s, "STOPPED")
	if _, err := s.Step(ctx, "stepi"); err != nil {
		t.Fatal(err)
	}
	waitSessionState(t, s, "STOPPED")
	if _, err := s.Step(ctx, "nexti"); err != nil {
		t.Fatal(err)
	}
	waitSessionState(t, s, "STOPPED")
	log := mustReadString(t, logPath)
	for _, want := range []string{"-exec-step-instruction", "-exec-next-instruction"} {
		if !strings.Contains(log, want) {
			t.Fatalf("missing %s in command log %q", want, log)
		}
	}
	_, _ = s.Detach(ctx)
}

func TestBacktraceDecodesMIStackFrameWrappers(t *testing.T) {
	rec, ok, err := gdbmi.ParseRecord(`1^done,stack=[frame={level="0",addr="0xb6997c24",func="select",from="/lib/libc.so.6"},frame={level="1",addr="0x12345678",func="foo",file="foo.c",fullname="/src/foo.c",line="42"}]`)
	if err != nil || !ok {
		t.Fatalf("parse ok=%v err=%v", ok, err)
	}
	frames, err := framesFromRecord(rec)
	if err != nil {
		t.Fatal(err)
	}
	if len(frames) != 2 {
		t.Fatalf("frames=%#v", frames)
	}
	if frames[0]["level"] != "0" || frames[0]["address"] != "0xb6997c24" || frames[0]["function"] != "select" || frames[0]["from"] != "/lib/libc.so.6" {
		t.Fatalf("bad frame 0: %#v", frames[0])
	}
	if frames[1]["level"] != "1" || frames[1]["address"] != "0x12345678" || frames[1]["function"] != "foo" || frames[1]["file"] != "foo.c" || frames[1]["fullname"] != "/src/foo.c" || frames[1]["line"] != "42" {
		t.Fatalf("bad frame 1: %#v", frames[1])
	}

	rec, ok, err = gdbmi.ParseRecord(`2^done,stack=[frame={level="0",addr="0x1234"}]`)
	if err != nil || !ok {
		t.Fatalf("minimal parse ok=%v err=%v", ok, err)
	}
	frames, err = framesFromRecord(rec)
	if err != nil {
		t.Fatal(err)
	}
	if len(frames) != 1 || frames[0]["level"] != "0" || frames[0]["address"] != "0x1234" {
		t.Fatalf("minimal frame not decoded: %#v", frames)
	}

	rec, ok, err = gdbmi.ParseRecord(`3^done,stack=[frame={func="missing"}]`)
	if err != nil || !ok {
		t.Fatalf("malformed parse ok=%v err=%v", ok, err)
	}
	if _, err := framesFromRecord(rec); err == nil || !strings.Contains(err.Error(), "GDB_MI_DATA_ERROR") {
		t.Fatalf("malformed frame should fail explicitly, err=%v", err)
	}
}

func TestDebugRemoteDebugGDBServerArgv(t *testing.T) {
	t.Setenv("LMUX_DEBUG_FAKE_GDB", "1")
	dir := t.TempDir()
	sym := writeSymbols(t, dir)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	for _, tc := range []struct {
		name string
		mode string
		want int
	}{
		{name: "off", mode: "off", want: 0},
		{name: "gdbserver", mode: "gdbserver", want: 1},
	} {
		t.Run(tc.name, func(t *testing.T) {
			fhc := &fakeHC2{}
			m := NewManager(Config{
				RuntimeDir:      filepath.Join(dir, tc.name),
				GDBPath:         os.Args[0],
				TargetGDBServer: "/target/gdbserver",
				MaxSessions:     2,
			}, fhc, nil)
			s, err := m.Start(ctx, StartRequest{Mode: "launch", Symbols: sym, Program: "/bin/app", RemoteDebug: tc.mode})
			if err != nil {
				t.Fatal(err)
			}
			waitSessionState(t, s, "STOPPED")
			fhc.mu.Lock()
			argv := append([]string(nil), fhc.startArgv[0]...)
			fhc.mu.Unlock()
			got := 0
			for _, arg := range argv {
				if arg == "--remote-debug" {
					got++
				}
			}
			if got != tc.want {
				t.Fatalf("--remote-debug count=%d want %d argv=%q", got, tc.want, argv)
			}
			_, _ = s.Detach(ctx)
		})
	}
}

func TestDebugRemoteDebugGDBCommandBeforeTargetSelect(t *testing.T) {
	t.Setenv("LMUX_DEBUG_FAKE_GDB", "1")
	dir := t.TempDir()
	t.Setenv("LMUX_DEBUG_FAKE_GDB_COMMAND_LOG", filepath.Join(dir, "gdb.commands"))
	sym := writeSymbols(t, dir)
	m := NewManager(Config{
		RuntimeDir:      dir,
		GDBPath:         os.Args[0],
		TargetGDBServer: "/target/gdbserver",
		MaxSessions:     2,
	}, &fakeHC2{}, nil)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	s, err := m.Start(ctx, StartRequest{Mode: "launch", Symbols: sym, Program: "/bin/app", RemoteDebug: "gdb"})
	if err != nil {
		t.Fatal(err)
	}
	waitSessionState(t, s, "STOPPED")
	logBytes, err := os.ReadFile(filepath.Join(dir, "gdb.commands"))
	if err != nil {
		t.Fatal(err)
	}
	log := string(logBytes)
	timeoutCmd := `-interpreter-exec console "set remotetimeout 10"`
	asyncCmd := `-gdb-set mi-async on`
	remoteCmd := `-interpreter-exec console "set debug remote 1"`
	timeoutIdx := strings.Index(log, timeoutCmd)
	asyncIdx := strings.Index(log, asyncCmd)
	remoteIdx := strings.Index(log, remoteCmd)
	targetIdx := strings.Index(log, "-target-select")
	if timeoutIdx < 0 || asyncIdx < 0 || remoteIdx < 0 || targetIdx < 0 || timeoutIdx > targetIdx || asyncIdx > targetIdx || remoteIdx > targetIdx {
		t.Fatalf("remote timeout and debug commands must precede target-select, log=%q", log)
	}
	if s.Status()["remote_debug"] != "gdb" {
		t.Fatalf("remote_debug missing from status: %#v", s.Status())
	}
	_, _ = s.Detach(ctx)
}

func TestDebugRemoteTimeoutConfigAndOrdering(t *testing.T) {
	t.Setenv("LMUX_DEBUG_FAKE_GDB", "1")
	dir := t.TempDir()
	sym := writeSymbols(t, dir)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	defaultMgr := NewManager(Config{
		RuntimeDir:      filepath.Join(dir, "default"),
		GDBPath:         os.Args[0],
		TargetGDBServer: "/target/gdbserver",
		MaxSessions:     2,
	}, &fakeHC2{}, nil)
	if got := defaultMgr.Capabilities()["gdb_remote_timeout_seconds"]; got != DefaultGDBRemoteTimeoutSeconds {
		t.Fatalf("default remote timeout=%#v want %d", got, DefaultGDBRemoteTimeoutSeconds)
	}
	if got := defaultMgr.Capabilities()["mi_async"]; got != true {
		t.Fatalf("mi_async capability=%#v want true", got)
	}
	if got := defaultMgr.Capabilities()["interrupt_transport"]; got != "gdb-rsp" {
		t.Fatalf("interrupt transport=%#v", got)
	}
	if got := defaultMgr.Capabilities()["interrupt_stop_requires_wait"]; got != true {
		t.Fatalf("interrupt wait capability=%#v", got)
	}

	logPath := filepath.Join(dir, "explicit.commands")
	t.Setenv("LMUX_DEBUG_FAKE_GDB_COMMAND_LOG", logPath)
	explicitMgr := NewManager(Config{
		RuntimeDir:              filepath.Join(dir, "explicit"),
		GDBPath:                 os.Args[0],
		TargetGDBServer:         "/target/gdbserver",
		MaxSessions:             2,
		GDBRemoteTimeoutSeconds: 37,
	}, &fakeHC2{}, nil)
	s, err := explicitMgr.Start(ctx, StartRequest{Mode: "launch", Symbols: sym, Program: "/bin/app"})
	if err != nil {
		t.Fatal(err)
	}
	waitSessionState(t, s, "STOPPED")
	log := mustReadString(t, logPath)
	timeoutCmd := `-interpreter-exec console "set remotetimeout 37"`
	asyncCmd := `-gdb-set mi-async on`
	timeoutIdx := strings.Index(log, timeoutCmd)
	asyncIdx := strings.Index(log, asyncCmd)
	targetIdx := strings.Index(log, "-target-select")
	if timeoutIdx < 0 || asyncIdx < 0 || targetIdx < 0 || timeoutIdx > targetIdx || asyncIdx > targetIdx {
		t.Fatalf("remotetimeout and mi-async commands must precede target-select, log=%q", log)
	}
	if got := s.Status()["gdb_remote_timeout_seconds"]; got != 37 {
		t.Fatalf("status remote timeout=%#v want 37", got)
	}
	_, _ = s.Detach(ctx)

	for _, bad := range []int{-1, 301} {
		m := NewManager(Config{RuntimeDir: filepath.Join(dir, fmt.Sprintf("bad-%d", bad)), GDBPath: os.Args[0], TargetGDBServer: "/target/gdbserver", MaxSessions: 2, GDBRemoteTimeoutSeconds: bad}, &fakeHC2{}, nil)
		_, err := m.Start(ctx, StartRequest{Mode: "launch", Symbols: sym, Program: "/bin/app"})
		if err == nil || !strings.Contains(err.Error(), "gdb remote timeout") {
			t.Fatalf("timeout %d should be rejected, err=%v", bad, err)
		}
	}
}

func TestDebugSocketPathLifetimeAndEndpoint(t *testing.T) {
	t.Setenv("LMUX_DEBUG_FAKE_GDB", "1")
	dir := t.TempDir()
	t.Setenv("LMUX_DEBUG_FAKE_GDB_COMMAND_LOG", filepath.Join(dir, "gdb.commands"))
	sym := writeSymbols(t, dir)
	m := NewManager(Config{
		RuntimeDir:      dir,
		GDBPath:         os.Args[0],
		TargetGDBServer: "/target/gdbserver",
		MaxSessions:     2,
	}, &fakeHC2{}, nil)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	s, err := m.Start(ctx, StartRequest{Mode: "launch", Symbols: sym, Program: "/bin/app"})
	if err != nil {
		t.Fatal(err)
	}
	waitSessionState(t, s, "STOPPED")
	select {
	case <-s.connReady:
	case <-ctx.Done():
		t.Fatal("RSP accept did not become ready before the test context expired")
	}
	assertSocketPath(t, s.socketPath)
	logBytes, err := os.ReadFile(filepath.Join(dir, "gdb.commands"))
	if err != nil {
		t.Fatal(err)
	}
	var targetSelect string
	for _, line := range strings.Split(string(logBytes), "\n") {
		if strings.Contains(line, "-target-select") {
			targetSelect = line
			break
		}
	}
	if targetSelect == "" {
		t.Fatalf("missing target-select command in %q", string(logBytes))
	}
	if strings.Contains(targetSelect, "unix::") {
		t.Fatalf("target-select must use bare pathname, got %q", targetSelect)
	}
	if !strings.Contains(targetSelect, "-target-select remote ") || !strings.Contains(targetSelect, s.socketPath) {
		t.Fatalf("bad target-select endpoint: %q want %q", targetSelect, s.socketPath)
	}
	raw := string(s.gdb.LastTargetSelectBytes())
	wantRaw := "4-target-select remote " + s.socketPath + "\n"
	if raw != wantRaw {
		t.Fatalf("raw target-select bytes = %q want %q", raw, wantRaw)
	}
	if strings.Contains(raw, "unix::") || strings.Contains(raw, "\r") || strings.Contains(raw, strconv.Quote(s.socketPath)) {
		t.Fatalf("raw target-select contains forbidden syntax/CR: %q", raw)
	}
	if !strings.Contains(s.Status()["rsp_accept"].(string), "succeeded=true") {
		t.Fatalf("accept state not recorded as succeeded: %v", s.Status()["rsp_accept"])
	}
	if _, err := s.Detach(ctx); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Lstat(s.socketPath); !os.IsNotExist(err) {
		t.Fatalf("socket path should be removed after detach: %v", err)
	}
}

func TestDebugTargetSelectFailureDiagnostics(t *testing.T) {
	t.Setenv("LMUX_DEBUG_FAKE_GDB", "1")
	t.Setenv("LMUX_DEBUG_FAKE_GDB_TARGET_ERROR", "1")
	dir := t.TempDir()
	sym := writeSymbols(t, dir)
	m := NewManager(Config{RuntimeDir: dir, GDBPath: os.Args[0], TargetGDBServer: "/gdbserver", MaxSessions: 2}, &fakeHC2{}, nil)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	s, err := m.Start(ctx, StartRequest{Mode: "launch", Symbols: sym, Program: "/bin/app"})
	if err != nil {
		t.Fatal(err)
	}
	waitSessionState(t, s, "ERROR")
	msg := fmt.Sprint(s.Status()["last_error"])
	for _, want := range []string{
		"TARGET_SELECT_FAILED",
		"raw_target_select=\"4-target-select remote ",
		"accept=attempted=true succeeded=false",
		"socket_before={stage=before",
		"exists=true",
		"is_socket=true",
		"socket_after={stage=after",
	} {
		if !strings.Contains(msg, want) {
			t.Fatalf("target-select diagnostics missing %q in %s", want, msg)
		}
	}
	_ = s.Stop(ctx)
	assertDebugClean(t, m, dir)
}

func TestDebugTargetSelectDisappearedSocketDiagnostics(t *testing.T) {
	t.Setenv("LMUX_DEBUG_FAKE_GDB", "1")
	t.Setenv("LMUX_DEBUG_FAKE_GDB_UNLINK_TARGET_SOCKET", "1")
	dir := t.TempDir()
	sym := writeSymbols(t, dir)
	m := NewManager(Config{RuntimeDir: dir, GDBPath: os.Args[0], TargetGDBServer: "/gdbserver", MaxSessions: 2}, &fakeHC2{}, nil)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	s, err := m.Start(ctx, StartRequest{Mode: "launch", Symbols: sym, Program: "/bin/app"})
	if err != nil {
		t.Fatal(err)
	}
	waitSessionState(t, s, "ERROR")
	msg := fmt.Sprint(s.Status()["last_error"])
	for _, want := range []string{
		"RSP_SOCKET_DISAPPEARED_DURING_TARGET_SELECT",
		"socket_before={stage=before",
		"exists=true",
		"socket_after={stage=after",
		"exists=false",
	} {
		if !strings.Contains(msg, want) {
			t.Fatalf("disappeared-socket diagnostics missing %q in %s", want, msg)
		}
	}
	_ = s.Stop(ctx)
	assertDebugClean(t, m, dir)
}

func TestDebugStartReturnsConnectingWhileTargetSelectOutlivesRequestContext(t *testing.T) {
	t.Setenv("LMUX_DEBUG_FAKE_GDB", "1")
	t.Setenv("LMUX_DEBUG_FAKE_GDB_TARGET_SELECT_DELAY_MS", "300")
	dir := t.TempDir()
	sym := writeSymbols(t, dir)
	m := NewManager(Config{RuntimeDir: dir, GDBPath: os.Args[0], TargetGDBServer: "/gdbserver", MaxSessions: 2}, &fakeHC2{}, nil)
	ctx, cancel := context.WithTimeout(context.Background(), 100*time.Millisecond)
	defer cancel()
	s, err := m.Start(ctx, StartRequest{Mode: "launch", Symbols: sym, Program: "/bin/app"})
	if err != nil {
		t.Fatal(err)
	}
	if st := s.Status(); st["state"] != "CONNECTING" {
		t.Fatalf("debug_start should return while target-select is connecting, got %#v", st)
	}
	<-ctx.Done()
	if m.ActiveCount() != 1 {
		t.Fatalf("session did not survive request context expiry")
	}
	waitSessionState(t, s, "STOPPED")
	if _, err := s.Detach(context.Background()); err != nil {
		t.Fatal(err)
	}
}

func TestRealGDBMultiarchProductionPathAcceptsUnixSocket(t *testing.T) {
	const gdb = "/usr/bin/gdb-multiarch"
	if _, err := os.Stat(gdb); err != nil {
		t.Skipf("%s not available: %v", gdb, err)
	}
	runtimeDir := filepath.Join(t.TempDir(), "lmux", "mcp")
	m := NewManager(Config{RuntimeDir: runtimeDir, GDBPath: gdb, TargetGDBServer: "/fake/gdbserver", MaxSessions: 2}, &fakeHC2{}, nil)
	token := "d-real-gdb"
	s := &Session{
		mgr:        m,
		Token:      token,
		Mode:       "launch",
		Symbols:    os.Args[0],
		Program:    "/bin/app",
		State:      "STARTING",
		socketPath: filepath.Join(runtimeDir, "debug", token+".sock"),
		toGDB:      make(chan []byte, MaxRSPQueueChunks),
		connReady:  make(chan struct{}),
		done:       make(chan struct{}),
		execDone:   make(chan struct{}),
		wait:       make(chan struct{}),
	}
	if err := os.MkdirAll(filepath.Dir(s.socketPath), 0700); err != nil {
		t.Fatal(err)
	}
	m.mu.Lock()
	m.sessions[token] = s
	m.mu.Unlock()
	ctx, cancel := context.WithCancel(context.Background())
	err := s.start(ctx)
	if err != nil {
		cancel()
		t.Fatal(err)
	}
	cancel()
	waitUntil(t, func() bool { return s.acceptAttempted && s.acceptSucceeded }, "real GDB accept")
	if !s.acceptAttempted || !s.acceptSucceeded {
		t.Fatalf("real gdb did not connect to production listener: attempted=%t succeeded=%t err=%q startErr=%v",
			s.acceptAttempted, s.acceptSucceeded, s.acceptError, err)
	}
	raw := string(s.gdb.LastTargetSelectBytes())
	if !strings.Contains(raw, "-target-select remote ") || !strings.Contains(raw, s.socketPath) || strings.Contains(raw, "unix::") || strings.Contains(raw, "\r") {
		t.Fatalf("bad real-gdb target-select bytes: %q path=%s err=%v", raw, s.socketPath, err)
	}
	if snap := s.socketSnapshot("real-gdb-after"); !snap.Exists || !snap.IsSocket {
		t.Fatalf("socket path missing before cleanup after real-gdb test: %s err=%v", snap.String(), err)
	}
	<-ctx.Done()
	if s.GDBPID == 0 || s.gdb == nil {
		t.Fatal("real GDB process did not survive initiating context lifetime")
	}
	st := s.Status()
	if st["state"] != "CONNECTING" {
		t.Fatalf("target-select should still be session-owned after caller context expires: %#v", st)
	}
	s.cleanup(context.Background(), true, true)
}

func TestDebugCapacity(t *testing.T) {
	dir := t.TempDir()
	sym := dir + "/a.out"
	if err := os.WriteFile(sym, []byte("symbols"), 0600); err != nil {
		t.Fatal(err)
	}
	fhc := &fakeHC2{}
	m := NewManager(Config{RuntimeDir: dir, TargetGDBServer: "/gdbserver", MaxSessions: 2, CommandActiveFunc: func() int { return 4 }}, fhc, nil)
	if _, err := m.Start(context.Background(), StartRequest{Mode: "launch", Symbols: sym, Program: "/bin/app"}); err == nil || !strings.Contains(err.Error(), "RESOURCE_LIMIT") {
		t.Fatalf("expected resource limit, got %v", err)
	}
}

func TestMultipleDebugSessionsAndRouting(t *testing.T) {
	t.Setenv("LMUX_DEBUG_FAKE_GDB", "1")
	dir := t.TempDir()
	sym := writeSymbols(t, dir)
	fhc := &fakeHC2{}
	var journalMu sync.Mutex
	var journal []string
	m := NewManager(Config{
		RuntimeDir:      dir,
		GDBPath:         os.Args[0],
		TargetGDBServer: "/target/gdbserver",
		MaxSessions:     2,
	}, fhc, func(event string, fields map[string]any) {
		journalMu.Lock()
		defer journalMu.Unlock()
		journal = append(journal, event)
	})
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	a, err := m.Start(ctx, StartRequest{Mode: "launch", Symbols: sym, Program: "/bin/a", Label: "A"})
	if err != nil {
		t.Fatal(err)
	}
	b, err := m.Start(ctx, StartRequest{Mode: "launch", Symbols: sym, Program: "/bin/b", Label: "B"})
	if err != nil {
		t.Fatal(err)
	}
	if a.Token == b.Token || a.ExecRef == b.ExecRef || a.socketPath == b.socketPath || a.GDBPID == b.GDBPID {
		t.Fatalf("debug sessions not isolated: a=%#v b=%#v", a.summary(), b.summary())
	}
	if !waitForStdin(t, fhc, a.ExecRef, "gdb-to-target") || !waitForStdin(t, fhc, b.ExecRef, "gdb-to-target") {
		t.Fatalf("fake GDB traffic did not route to both target execs")
	}
	if !m.ExecOutput("stderr", b.ExecRef, []byte("stderr-B")) {
		t.Fatal("debug stderr was not claimed")
	}
	diag, err := b.DiagnosticsRead(DiagnosticsRequest{Source: "gdbserver_stderr", Contains: "stderr-B"})
	if err != nil {
		t.Fatal(err)
	}
	if len(diag["items"].([]map[string]any)) != 1 {
		t.Fatalf("debug stderr did not reach session B diagnostics")
	}
	if m.ExecOutput("stdout", 999999, []byte("command-output")) {
		t.Fatal("unknown command ref was incorrectly claimed by debug manager")
	}
	if err := a.Stop(ctx); err != nil {
		t.Fatal(err)
	}
	if m.ActiveCount() != 1 {
		t.Fatalf("stopping A should leave B active, active=%d", m.ActiveCount())
	}
	if _, ok := m.Get(b.Token); !ok {
		t.Fatal("session B disappeared when session A stopped")
	}
	if _, err := b.Detach(ctx); err != nil {
		t.Fatal(err)
	}
	journalMu.Lock()
	gotJournal := strings.Join(journal, ",")
	journalMu.Unlock()
	for _, want := range []string{"DEBUG_START", "DEBUG_CONNECTED", "DEBUG_STOP", "DEBUG_DETACH"} {
		if !strings.Contains(gotJournal, want) {
			t.Fatalf("journal missing %s in %s", want, gotJournal)
		}
	}
}

func TestDebugCapacityCombinations(t *testing.T) {
	t.Setenv("LMUX_DEBUG_FAKE_GDB", "1")
	dir := t.TempDir()
	sym := writeSymbols(t, dir)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	fhc := &fakeHC2{}
	m := NewManager(Config{RuntimeDir: dir, GDBPath: os.Args[0], TargetGDBServer: "/gdbserver", MaxSessions: 2}, fhc, nil)
	a, err := m.Start(ctx, StartRequest{Mode: "launch", Symbols: sym, Program: "/bin/a"})
	if err != nil {
		t.Fatal(err)
	}
	b, err := m.Start(ctx, StartRequest{Mode: "launch", Symbols: sym, Program: "/bin/b"})
	if err != nil {
		t.Fatal(err)
	}
	if _, err := m.Start(ctx, StartRequest{Mode: "launch", Symbols: sym, Program: "/bin/c"}); err == nil || !strings.Contains(err.Error(), "RESOURCE_LIMIT") {
		t.Fatalf("expected max debug sessions resource limit, got %v", err)
	}
	_, _ = a.Detach(ctx)
	_, _ = b.Detach(ctx)

	commandActive := 3
	m = NewManager(Config{RuntimeDir: dir, GDBPath: os.Args[0], TargetGDBServer: "/gdbserver", MaxSessions: 2, CommandActiveFunc: func() int { return commandActive }}, &fakeHC2{}, nil)
	a, err = m.Start(ctx, StartRequest{Mode: "launch", Symbols: sym, Program: "/bin/a"})
	if err != nil {
		t.Fatalf("1 debug + 3 commands should be allowed: %v", err)
	}
	if _, err := m.Start(ctx, StartRequest{Mode: "launch", Symbols: sym, Program: "/bin/b"}); err == nil || !strings.Contains(err.Error(), "RESOURCE_LIMIT") {
		t.Fatalf("expected target capacity resource limit, got %v", err)
	}
	_, _ = a.Detach(ctx)
}

func TestDebugStartFailureCleanup(t *testing.T) {
	t.Setenv("LMUX_DEBUG_FAKE_GDB", "1")
	dir := t.TempDir()
	sym := writeSymbols(t, dir)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	cases := []struct {
		name string
		cfg  Config
		hc   *fakeHC2
		env  map[string]string
	}{
		{name: "gdbserver start", cfg: Config{RuntimeDir: dir, GDBPath: os.Args[0], TargetGDBServer: "/gdbserver", MaxSessions: 2}, hc: &fakeHC2{failStart: true}},
		{name: "gdb spawn", cfg: Config{RuntimeDir: dir, GDBPath: dir + "/missing-gdb", TargetGDBServer: "/gdbserver", MaxSessions: 2}, hc: &fakeHC2{}},
	}
	for _, tt := range cases {
		t.Run(tt.name, func(t *testing.T) {
			for k, v := range tt.env {
				t.Setenv(k, v)
			}
			m := NewManager(tt.cfg, tt.hc, nil)
			if _, err := m.Start(ctx, StartRequest{Mode: "launch", Symbols: sym, Program: "/bin/app"}); err == nil {
				t.Fatal("expected start failure")
			}
			if m.ActiveCount() != 0 {
				t.Fatalf("debug session leaked after %s", tt.name)
			}
			m.mu.Lock()
			refCount := len(m.byRef)
			m.mu.Unlock()
			if refCount != 0 {
				t.Fatalf("exec ownership leaked after %s", tt.name)
			}
			if entries, _ := os.ReadDir(filepath.Join(dir, "debug")); len(entries) != 0 {
				t.Fatalf("socket path leaked after %s: %v", tt.name, entries)
			}
		})
	}
}

func TestDebugStartTargetExitedWinsOverClosedPipe(t *testing.T) {
	t.Setenv("LMUX_DEBUG_FAKE_GDB", "1")
	t.Setenv("LMUX_DEBUG_FAKE_GDB_TARGET_SELECT_DELAY_MS", "80")
	dir := t.TempDir()
	sym := writeSymbols(t, dir)
	var m *Manager
	fhc := &fakeHC2{}
	fhc.onStart = func(ref uint64) {
		time.Sleep(10 * time.Millisecond)
		m.ExecOutput("stderr", ref, []byte("TARGET_GDBSERVER_ERROR"))
		m.ExecState(hc2.ExecState{Ref: ref, State: 4, ExitCode: 42})
	}
	m = NewManager(Config{RuntimeDir: dir, GDBPath: os.Args[0], TargetGDBServer: "/gdbserver", MaxSessions: 2}, fhc, nil)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	s, err := m.Start(ctx, StartRequest{Mode: "launch", Symbols: sym, Program: "/bin/app"})
	if err != nil {
		t.Fatal(err)
	}
	waitSessionState(t, s, "LOST")
	msg := fmt.Sprint(s.Status()["last_error"])
	if !strings.Contains(msg, "GDBSERVER_EXITED") || !strings.Contains(msg, "TARGET_GDBSERVER_ERROR") || strings.Contains(msg, "closed pipe") {
		t.Fatalf("expected gdbserver root cause, got %v", msg)
	}
	_ = s.Stop(ctx)
	assertDebugClean(t, m, dir)
}

func TestDebugStartHostGDBExitedProvenance(t *testing.T) {
	t.Setenv("LMUX_DEBUG_FAKE_GDB", "1")
	t.Setenv("LMUX_DEBUG_FAKE_GDB_EXIT_IMMEDIATE", "1")
	dir := t.TempDir()
	sym := writeSymbols(t, dir)
	m := NewManager(Config{RuntimeDir: dir, GDBPath: os.Args[0], TargetGDBServer: "/gdbserver", MaxSessions: 2}, &fakeHC2{}, nil)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	_, err := m.Start(ctx, StartRequest{Mode: "launch", Symbols: sym, Program: "/bin/app"})
	if err == nil || !strings.Contains(err.Error(), "GDB_") || !strings.Contains(err.Error(), "HOST_GDB_DIED") {
		t.Fatalf("expected host GDB provenance, got %v", err)
	}
	assertDebugClean(t, m, dir)
}

func TestDebugStartRSPTargetStdinClosed(t *testing.T) {
	t.Setenv("LMUX_DEBUG_FAKE_GDB", "1")
	t.Setenv("LMUX_DEBUG_FAKE_GDB_TARGET_SELECT_DELAY_MS", "50")
	dir := t.TempDir()
	sym := writeSymbols(t, dir)
	m := NewManager(Config{RuntimeDir: dir, GDBPath: os.Args[0], TargetGDBServer: "/gdbserver", MaxSessions: 2}, &fakeHC2{stdinErr: io.ErrClosedPipe}, nil)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	s, err := m.Start(ctx, StartRequest{Mode: "launch", Symbols: sym, Program: "/bin/app"})
	if err != nil {
		t.Fatal(err)
	}
	waitSessionState(t, s, "LOST")
	if msg := fmt.Sprint(s.Status()["last_error"]); !strings.Contains(msg, "RSP_TARGET_STDIN_CLOSED") {
		t.Fatalf("expected target stdin closed provenance, got %v", msg)
	}
	_ = s.Stop(ctx)
	assertDebugClean(t, m, dir)
}

func TestDebugRSPLocalPeerClosedProvenance(t *testing.T) {
	t.Setenv("LMUX_DEBUG_FAKE_GDB", "1")
	t.Setenv("LMUX_DEBUG_FAKE_GDB_RSP_CLOSE", "1")
	dir := t.TempDir()
	sym := writeSymbols(t, dir)
	m := NewManager(Config{RuntimeDir: dir, GDBPath: os.Args[0], TargetGDBServer: "/gdbserver", MaxSessions: 2}, &fakeHC2{}, nil)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	s, err := m.Start(ctx, StartRequest{Mode: "launch", Symbols: sym, Program: "/bin/app"})
	if err != nil {
		t.Fatal(err)
	}
	waitSessionState(t, s, "STOPPED")
	waitUntil(t, func() bool {
		return strings.Contains(fmt.Sprint(s.Status()["local_rsp_close"]), "RSP_LOCAL_CLOSED")
	}, "connecting local RSP close diagnostic")
	st := s.Status()
	if !strings.Contains(fmt.Sprint(st["local_rsp_close"]), "RSP_LOCAL_CLOSED") {
		t.Fatalf("expected connecting local RSP close to be secondary diagnostic, status=%v", st)
	}
	_ = s.Stop(ctx)
}

func TestDebugConnectingRSPCloseThenTargetSelectErrorWins(t *testing.T) {
	t.Setenv("LMUX_DEBUG_FAKE_GDB", "1")
	t.Setenv("LMUX_DEBUG_FAKE_GDB_RSP_CLOSE_THEN_ERROR", "1")
	dir := t.TempDir()
	sym := writeSymbols(t, dir)
	fhc := &fakeHC2{}
	m := NewManager(Config{RuntimeDir: dir, GDBPath: os.Args[0], TargetGDBServer: "/gdbserver", MaxSessions: 2}, fhc, nil)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	s, err := m.Start(ctx, StartRequest{Mode: "launch", Symbols: sym, Program: "/bin/app"})
	if err != nil {
		t.Fatal(err)
	}
	st := waitSessionState(t, s, "ERROR")
	msg := fmt.Sprint(st["last_error"])
	if !strings.Contains(msg, "TARGET_SELECT_FAILED") || !strings.Contains(msg, "GDB_ERROR: remote closed") || strings.HasPrefix(msg, "RSP_LOCAL_CLOSED") {
		t.Fatalf("target-select error should win over local close, status=%#v", st)
	}
	if !strings.Contains(fmt.Sprint(st["local_rsp_close"]), "RSP_LOCAL_CLOSED") {
		t.Fatalf("missing secondary local close diagnostic: %#v", st)
	}
	waitDebugCleaned(t, m)
	assertTombstone(t, m, s.Token)
	assertReleased(t, fhc, s.ExecRef)
	_ = s.Stop(ctx)
	assertDebugClean(t, m, dir)
}

func TestDebugConnectingRSPCloseThenGDBExitWins(t *testing.T) {
	t.Setenv("LMUX_DEBUG_FAKE_GDB", "1")
	t.Setenv("LMUX_DEBUG_FAKE_GDB_RSP_CLOSE_THEN_EXIT", "1")
	dir := t.TempDir()
	sym := writeSymbols(t, dir)
	fhc := &fakeHC2{}
	m := NewManager(Config{RuntimeDir: dir, GDBPath: os.Args[0], TargetGDBServer: "/gdbserver", MaxSessions: 2}, fhc, nil)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	s, err := m.Start(ctx, StartRequest{Mode: "launch", Symbols: sym, Program: "/bin/app"})
	if err != nil {
		t.Fatal(err)
	}
	st := waitSessionState(t, s, "LOST")
	msg := fmt.Sprint(st["last_error"])
	if !strings.Contains(msg, "GDB_EXITED") || strings.HasPrefix(msg, "RSP_LOCAL_CLOSED") {
		t.Fatalf("gdb exit should win over local close, status=%#v", st)
	}
	diag, err := s.DiagnosticsRead(DiagnosticsRequest{Source: "gdb_stderr", Contains: "HOST_GDB_EXIT_AFTER_RSP_CLOSE"})
	if err != nil {
		t.Fatal(err)
	}
	if len(diag["items"].([]map[string]any)) != 1 {
		t.Fatalf("missing host gdb stderr: %#v diag=%#v", st, diag)
	}
	waitDebugCleaned(t, m)
	assertTombstone(t, m, s.Token)
	assertReleased(t, fhc, s.ExecRef)
	_ = s.Stop(ctx)
	assertDebugClean(t, m, dir)
}

func TestDebugEstablishedRSPCloseIsTerminalAndAutoCleans(t *testing.T) {
	t.Setenv("LMUX_DEBUG_FAKE_GDB", "1")
	dir := t.TempDir()
	sym := writeSymbols(t, dir)
	fhc := &fakeHC2{}
	m := NewManager(Config{RuntimeDir: dir, GDBPath: os.Args[0], TargetGDBServer: "/gdbserver", MaxSessions: 2}, fhc, nil)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	s, err := m.Start(ctx, StartRequest{Mode: "launch", Symbols: sym, Program: "/bin/app"})
	if err != nil {
		t.Fatal(err)
	}
	waitSessionState(t, s, "STOPPED")
	waitUntil(t, func() bool {
		s.mu.Lock()
		defer s.mu.Unlock()
		return s.conn != nil
	}, "accepted RSP connection")
	s.mu.Lock()
	conn := s.conn
	s.mu.Unlock()
	if conn == nil {
		t.Fatal("missing accepted RSP connection")
	}
	_ = conn.Close()
	st := waitSessionState(t, s, "LOST")
	if msg := fmt.Sprint(st["last_error"]); !strings.Contains(msg, "RSP_LOCAL_CLOSED") {
		t.Fatalf("established local close should be terminal root cause, status=%#v", st)
	}
	waitDebugCleaned(t, m)
	assertTombstone(t, m, s.Token)
	assertReleased(t, fhc, s.ExecRef)
	if _, err := os.Lstat(s.socketPath); !os.IsNotExist(err) {
		t.Fatalf("socket path should be removed after terminal cleanup: %v", err)
	}
	if st := s.Status(); st["gdb_alive"] != false || st["gdbserver_alive"] != false {
		t.Fatalf("resources should be cleaned in tombstone status: %#v", st)
	}
	if err := s.Stop(ctx); err != nil {
		t.Fatal(err)
	}
	assertDebugClean(t, m, dir)
}

func TestDebugAttachTerminalCleanupDetachesBeforeDestructiveCleanup(t *testing.T) {
	t.Setenv("LMUX_DEBUG_FAKE_GDB", "1")
	dir := t.TempDir()
	logPath := filepath.Join(dir, "gdb.commands")
	t.Setenv("LMUX_DEBUG_FAKE_GDB_COMMAND_LOG", logPath)
	sym := writeSymbols(t, dir)
	fhc := &fakeHC2{}
	m := NewManager(Config{RuntimeDir: dir, GDBPath: os.Args[0], TargetGDBServer: "/gdbserver", MaxSessions: 2}, fhc, nil)
	ctx, cancel := context.WithTimeout(context.Background(), 6*time.Second)
	defer cancel()
	s, err := m.Start(ctx, StartRequest{Mode: "attach", Symbols: sym, PID: 75})
	if err != nil {
		t.Fatal(err)
	}
	waitSessionState(t, s, "STOPPED")
	ref := s.ExecRef
	go func() {
		deadline := time.Now().Add(3 * time.Second)
		for time.Now().Before(deadline) && !commandLogContains(logPath, "-target-detach") {
			time.Sleep(time.Millisecond)
		}
		m.ExecState(hc2.ExecState{Ref: ref, State: 4})
	}()
	s.fail("LOST", errors.New("forced terminal test"))
	waitDebugCleaned(t, m)
	assertTombstone(t, m, s.Token)
	if st := s.Status(); st["inferior_cleanup"] != "detached" {
		t.Fatalf("expected detached cleanup, status=%#v", st)
	}
	fhc.mu.Lock()
	signals := append([]byte(nil), fhc.signals...)
	releases := append([]uint64(nil), fhc.releases...)
	fhc.mu.Unlock()
	if len(signals) != 0 {
		t.Fatalf("detach path should not signal gdbserver, signals=%v", signals)
	}
	if len(releases) != 1 || releases[0] != ref {
		t.Fatalf("exec ref not released once: %v ref=%d", releases, ref)
	}
	if log := mustReadString(t, logPath); !strings.Contains(log, "-target-detach") {
		t.Fatalf("missing target detach command, log=%q", log)
	}
}

func TestDebugAttachConnectingCleanupUsesGDBServerEOFWhenGDBUnavailable(t *testing.T) {
	t.Setenv("LMUX_DEBUG_FAKE_GDB", "1")
	t.Setenv("LMUX_DEBUG_FAKE_GDB_RSP_CLOSE_THEN_EXIT", "1")
	dir := t.TempDir()
	sym := writeSymbols(t, dir)
	var m *Manager
	fhc := &fakeHC2{}
	fhc.onStdinClose = func(ref uint64) {
		m.ExecState(hc2.ExecState{Ref: ref, State: 4})
	}
	m = NewManager(Config{RuntimeDir: dir, GDBPath: os.Args[0], TargetGDBServer: "/gdbserver", MaxSessions: 2}, fhc, nil)
	ctx, cancel := context.WithTimeout(context.Background(), 6*time.Second)
	defer cancel()
	s, err := m.Start(ctx, StartRequest{Mode: "attach", Symbols: sym, PID: 75})
	if err != nil {
		t.Fatal(err)
	}
	st := waitSessionState(t, s, "LOST")
	if !strings.Contains(fmt.Sprint(st["last_error"]), "GDB_EXITED") {
		t.Fatalf("expected gdb exit root cause, status=%#v", st)
	}
	waitDebugCleaned(t, m)
	fhc.mu.Lock()
	stdinCloses := append([]uint64(nil), fhc.stdinCloses...)
	signals := append([]byte(nil), fhc.signals...)
	releases := append([]uint64(nil), fhc.releases...)
	fhc.mu.Unlock()
	if len(stdinCloses) != 1 || stdinCloses[0] != s.ExecRef {
		t.Fatalf("expected stdin EOF before release, closes=%v ref=%d", stdinCloses, s.ExecRef)
	}
	if len(signals) != 0 {
		t.Fatalf("graceful EOF cleanup should not signal gdbserver, signals=%v", signals)
	}
	if len(releases) != 1 || releases[0] != s.ExecRef {
		t.Fatalf("exec ref not released once: %v ref=%d", releases, s.ExecRef)
	}
	if st := s.Status(); st["inferior_cleanup"] != "graceful-gdbserver-exit" {
		t.Fatalf("expected graceful gdbserver exit cleanup, status=%#v", st)
	}
}

func TestDebugAttachCleanupEscalatesWhenGracefulEOFDoesNotExit(t *testing.T) {
	t.Setenv("LMUX_DEBUG_FAKE_GDB", "1")
	t.Setenv("LMUX_DEBUG_FAKE_GDB_RSP_CLOSE_THEN_EXIT", "1")
	dir := t.TempDir()
	sym := writeSymbols(t, dir)
	fhc := &fakeHC2{}
	m := NewManager(Config{RuntimeDir: dir, GDBPath: os.Args[0], TargetGDBServer: "/gdbserver", MaxSessions: 2}, fhc, nil)
	ctx, cancel := context.WithTimeout(context.Background(), 8*time.Second)
	defer cancel()
	s, err := m.Start(ctx, StartRequest{Mode: "attach", Symbols: sym, PID: 75})
	if err != nil {
		t.Fatal(err)
	}
	waitSessionState(t, s, "LOST")
	waitDebugCleaned(t, m)
	fhc.mu.Lock()
	stdinCloses := append([]uint64(nil), fhc.stdinCloses...)
	signals := append([]byte(nil), fhc.signals...)
	fhc.mu.Unlock()
	if len(stdinCloses) != 1 {
		t.Fatalf("expected stdin EOF attempt, closes=%v", stdinCloses)
	}
	if len(signals) < 2 || signals[0] != hc2.SigTERM || signals[1] != hc2.SigKILL {
		t.Fatalf("expected TERM/KILL fallback, signals=%v", signals)
	}
	if st := s.Status(); st["inferior_cleanup"] != "forced-unconfirmed" {
		t.Fatalf("expected forced-unconfirmed cleanup, status=%#v", st)
	}
}

func TestDebugLaunchTerminalCleanupDoesNotUseAttachDetachPath(t *testing.T) {
	t.Setenv("LMUX_DEBUG_FAKE_GDB", "1")
	dir := t.TempDir()
	logPath := filepath.Join(dir, "gdb.commands")
	t.Setenv("LMUX_DEBUG_FAKE_GDB_COMMAND_LOG", logPath)
	sym := writeSymbols(t, dir)
	fhc := &fakeHC2{}
	m := NewManager(Config{RuntimeDir: dir, GDBPath: os.Args[0], TargetGDBServer: "/gdbserver", MaxSessions: 2}, fhc, nil)
	ctx, cancel := context.WithTimeout(context.Background(), 6*time.Second)
	defer cancel()
	s, err := m.Start(ctx, StartRequest{Mode: "launch", Symbols: sym, Program: "/bin/app"})
	if err != nil {
		t.Fatal(err)
	}
	waitSessionState(t, s, "STOPPED")
	s.fail("LOST", errors.New("launch terminal test"))
	waitDebugCleaned(t, m)
	if log := mustReadString(t, logPath); strings.Contains(log, "-target-detach") {
		t.Fatalf("launch terminal cleanup should not target-detach, log=%q", log)
	}
	assertReleased(t, fhc, s.ExecRef)
}

func TestDebugDiagnosticsReadableAfterTerminalCleanup(t *testing.T) {
	t.Setenv("LMUX_DEBUG_FAKE_GDB", "1")
	dir := t.TempDir()
	sym := writeSymbols(t, dir)
	fhc := &fakeHC2{}
	m := NewManager(Config{RuntimeDir: dir, GDBPath: os.Args[0], TargetGDBServer: "/gdbserver", MaxSessions: 2}, fhc, nil)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	s, err := m.Start(ctx, StartRequest{Mode: "launch", Symbols: sym, Program: "/bin/app"})
	if err != nil {
		t.Fatal(err)
	}
	waitSessionState(t, s, "STOPPED")
	if !m.ExecOutput("stderr", s.ExecRef, []byte("TOMBSTONE_DIAG\n")) {
		t.Fatal("debug stderr was not claimed")
	}
	if !m.ExecLost(s.ExecRef, 99) {
		t.Fatal("debug lost was not claimed")
	}
	waitSessionState(t, s, "LOST")
	waitDebugCleaned(t, m)
	assertTombstone(t, m, s.Token)
	assertReleasedOnly(t, fhc, s.ExecRef)
	diag, err := s.DiagnosticsRead(DiagnosticsRequest{Source: "gdbserver_stderr", Contains: "TOMBSTONE_DIAG"})
	if err != nil {
		t.Fatal(err)
	}
	if len(diag["items"].([]map[string]any)) != 1 {
		t.Fatalf("terminal tombstone diagnostics missing: %#v", diag)
	}
}

func TestRSPBridgeTargetToGDBExactProblemSequence(t *testing.T) {
	fhc := &fakeHC2{}
	m, s := newBridgeTestSession(fhc)
	c1, c2 := net.Pipe()
	defer c2.Close()
	s.mu.Lock()
	s.conn = c1
	s.mu.Unlock()
	closeOnce(s.connReady)
	go s.writeToGDB()
	chunks := []string{"+$vCon", "t;c;", "C;t;s;S#11", "+", "$#", "00"}
	expectedOnce := "+$vCont;c;C;t;s;S#11+$#00"
	const iterations = 200
	readDone := readPipeExactAsync(c2, len(expectedOnce)*iterations)
	for i := 0; i < iterations; i++ {
		for _, chunk := range chunks {
			if !m.ExecOutput("stdout", s.ExecRef, []byte(chunk)) {
				t.Fatal("debug stdout was not claimed")
			}
		}
	}
	got := waitReadExact(t, readDone)
	if string(got) != strings.Repeat(expectedOnce, iterations) {
		t.Fatalf("T2G stream reordered/corrupted: got %q want prefix %q", string(got[:minInt(len(got), 80)]), expectedOnce)
	}
	diag, err := s.DiagnosticsRead(DiagnosticsRequest{Source: "rsp", MaxEvents: len(chunks)})
	if err != nil {
		t.Fatal(err)
	}
	trace := diag["items"].([]map[string]any)
	if len(trace) < len(chunks) || trace[0]["direction"] != "T2G" || trace[0]["seq"] != uint64(1) || trace[0]["escaped"] != strconv.QuoteToASCII(chunks[0]) {
		t.Fatalf("unexpected T2G trace prefix: %#v", trace[:minInt(len(trace), len(chunks))])
	}
	closeOnce(s.done)
	_ = c1.Close()
}

func TestDebugDiagnosticsRSPPaginationAndContains(t *testing.T) {
	fhc := &fakeHC2{}
	m, s := newBridgeTestSession(fhc)
	c1, c2 := net.Pipe()
	defer c2.Close()
	s.mu.Lock()
	s.conn = c1
	s.mu.Unlock()
	closeOnce(s.connReady)
	go s.writeToGDB()
	chunks := []string{
		"+$qSupported:multiprocess+#00",
		"+$vCont;c;C;t;s;S#11",
		"+$vMustReplyEmpty#00",
	}
	readDone := readPipeExactAsync(c2, len(strings.Join(chunks, "")))
	for _, chunk := range chunks {
		if !m.ExecOutput("stdout", s.ExecRef, []byte(chunk)) {
			t.Fatal("debug stdout was not claimed")
		}
	}
	_ = waitReadExact(t, readDone)
	waitUntil(t, func() bool {
		return s.Status()["rsp_trace_entries"] == 3
	}, "rsp trace entries")
	page1, err := s.DiagnosticsRead(DiagnosticsRequest{Source: "rsp", MaxEvents: 2})
	if err != nil {
		t.Fatal(err)
	}
	items1 := page1["items"].([]map[string]any)
	if len(items1) != 2 || page1["at_end"] == true {
		t.Fatalf("bad first rsp page: %#v", page1)
	}
	page2, err := s.DiagnosticsRead(DiagnosticsRequest{Source: "rsp", Cursor: page1["next_cursor"].(string), MaxEvents: 2})
	if err != nil {
		t.Fatal(err)
	}
	items2 := page2["items"].([]map[string]any)
	if len(items2) != 1 || items2[0]["seq"] == items1[len(items1)-1]["seq"] {
		t.Fatalf("bad second rsp page: page1=%#v page2=%#v", page1, page2)
	}
	found, err := s.DiagnosticsRead(DiagnosticsRequest{Source: "rsp", Contains: "vMustReplyEmpty"})
	if err != nil {
		t.Fatal(err)
	}
	matches := found["items"].([]map[string]any)
	if len(matches) != 1 || !strings.Contains(matches[0]["escaped"].(string), "vMustReplyEmpty") {
		t.Fatalf("bad rsp contains result: %#v", found)
	}
	closeOnce(s.done)
	_ = c1.Close()
}

func TestRSPBridgePreAcceptOutputOrdering(t *testing.T) {
	fhc := &fakeHC2{}
	m, s := newBridgeTestSession(fhc)
	go s.writeToGDB()
	if !m.ExecOutput("stdout", s.ExecRef, []byte("A-before-accept|")) {
		t.Fatal("pre-accept output was not claimed")
	}
	c1, c2 := net.Pipe()
	defer c2.Close()
	readDone := readPipeExactAsync(c2, len("A-before-accept|B-after-accept|"))
	s.mu.Lock()
	s.conn = c1
	s.mu.Unlock()
	closeOnce(s.connReady)
	if !m.ExecOutput("stdout", s.ExecRef, []byte("B-after-accept|")) {
		t.Fatal("post-accept output was not claimed")
	}
	got := waitReadExact(t, readDone)
	if string(got) != "A-before-accept|B-after-accept|" {
		t.Fatalf("pre-accept output reordered: %q", string(got))
	}
	closeOnce(s.done)
	_ = c1.Close()
}

func TestRSPBridgeExecOutputCopiesReusableBufferBeforeAsyncWrite(t *testing.T) {
	fhc := &fakeHC2{}
	m, s := newBridgeTestSession(fhc)
	go s.writeToGDB()
	buf := []byte("AAA")
	if !m.ExecOutput("stdout", s.ExecRef, buf) {
		t.Fatal("first debug stdout was not claimed")
	}
	copy(buf, "BBB")
	if !m.ExecOutput("stdout", s.ExecRef, buf) {
		t.Fatal("second debug stdout was not claimed")
	}
	c1, c2 := net.Pipe()
	defer c2.Close()
	readDone := readPipeExactAsync(c2, len("AAABBB"))
	s.mu.Lock()
	s.conn = c1
	s.mu.Unlock()
	closeOnce(s.connReady)
	got := waitReadExact(t, readDone)
	if string(got) != "AAABBB" {
		t.Fatalf("debug async path retained borrowed payload: got %q want AAABBB", string(got))
	}
	closeOnce(s.done)
	_ = c1.Close()
}

func TestRSPBridgeGDBWriterHandlesPartialWrites(t *testing.T) {
	fhc := &fakeHC2{}
	m, s := newBridgeTestSession(fhc)
	conn := &partialWriteConn{max: 3}
	s.mu.Lock()
	s.conn = conn
	s.mu.Unlock()
	closeOnce(s.connReady)
	go s.writeToGDB()
	payload := []byte("ABCDEFGHIJ")
	if !m.ExecOutput("stdout", s.ExecRef, payload) {
		t.Fatal("debug stdout was not claimed")
	}
	waitUntil(t, func() bool { return string(conn.bytes()) == string(payload) }, "partial writes to finish")
	st := s.Status()
	if got := st["rsp_gdb_tx_bytes"]; got != uint64(len(payload)) {
		t.Fatalf("rsp_gdb_tx_bytes=%#v want %d", got, len(payload))
	}
	if got := st["rsp_pending_to_gdb_bytes"]; got != uint64(0) {
		t.Fatalf("rsp_pending_to_gdb_bytes=%#v want 0", got)
	}
	diag, err := s.DiagnosticsRead(DiagnosticsRequest{Source: "rsp"})
	if err != nil {
		t.Fatal(err)
	}
	trace := diag["items"].([]map[string]any)
	var got bytes.Buffer
	for _, ent := range trace {
		if ent["direction"] != "T2G" {
			continue
		}
		raw, err := strconv.Unquote(ent["escaped"].(string))
		if err != nil {
			t.Fatalf("bad escaped trace entry %#v: %v", ent, err)
		}
		got.WriteString(raw)
	}
	if got.String() != string(payload) {
		t.Fatalf("partial trace duplicated or dropped bytes: got %q want %q", got.String(), string(payload))
	}
	closeOnce(s.done)
	_ = conn.Close()
}

func TestRSPBridgeRetriesTargetStdinBackpressureExactly(t *testing.T) {
	bp := backpressureErr()
	fhc := &fakeHC2{
		stdinErrs:      []error{nil, bp, bp, nil, nil},
		acceptedCh:     make(chan []byte, 8),
		backpressureCh: make(chan struct{}, 8),
	}
	m, s := newBridgeTestSession(fhc)
	go s.writeTargetStdin()

	if !s.enqueueRSP([]byte("A")) {
		t.Fatal("enqueue A failed")
	}
	waitAccepted(t, fhc.acceptedCh, "A")
	if !s.enqueueRSP([]byte("B")) {
		t.Fatal("enqueue B failed")
	}
	waitBackpressure(t, fhc.backpressureCh)
	if !m.ExecStdinReady(s.ExecRef) {
		t.Fatal("stdin ready was not claimed")
	}
	waitBackpressure(t, fhc.backpressureCh)
	if !m.ExecStdinReady(s.ExecRef) {
		t.Fatal("stdin ready was not claimed")
	}
	waitAccepted(t, fhc.acceptedCh, "B")
	if !s.enqueueRSP([]byte("C")) {
		t.Fatal("enqueue C failed")
	}
	waitAccepted(t, fhc.acceptedCh, "C")

	fhc.mu.Lock()
	got := string(fhc.stdin[s.ExecRef])
	fhc.mu.Unlock()
	if got != "ABC" {
		t.Fatalf("target stdin = %q want ABC", got)
	}
	diag, err := s.DiagnosticsRead(DiagnosticsRequest{Source: "rsp"})
	if err != nil {
		t.Fatal(err)
	}
	trace := diag["items"].([]map[string]any)
	if len(trace) < 3 || trace[0]["escaped"] != strconv.QuoteToASCII("A") || trace[1]["escaped"] != strconv.QuoteToASCII("B") || trace[2]["escaped"] != strconv.QuoteToASCII("C") {
		t.Fatalf("G2T trace did not preserve A/B/C order: %#v", trace)
	}
	closeOnce(s.done)
}

func TestDebugDiagnosticsTextPaginationAndContains(t *testing.T) {
	fhc := &fakeHC2{}
	m, s := newBridgeTestSession(fhc)
	if !m.ExecOutput("stderr", s.ExecRef, []byte("alpha\nvCont packet\nvMustReplyEmpty packet\nomega\n")) {
		t.Fatal("debug stderr was not claimed")
	}
	page1, err := s.DiagnosticsRead(DiagnosticsRequest{Source: "gdbserver_stderr", MaxEvents: 2, MaxBytes: 64})
	if err != nil {
		t.Fatal(err)
	}
	items1 := page1["items"].([]map[string]any)
	if len(items1) != 2 || page1["at_end"] == true {
		t.Fatalf("bad first stderr page: %#v", page1)
	}
	page2, err := s.DiagnosticsRead(DiagnosticsRequest{Source: "gdbserver_stderr", Cursor: page1["next_cursor"].(string), MaxEvents: 4})
	if err != nil {
		t.Fatal(err)
	}
	items2 := page2["items"].([]map[string]any)
	if len(items2) != 2 || items2[0]["offset"] == items1[1]["offset"] {
		t.Fatalf("bad second stderr page: page1=%#v page2=%#v", page1, page2)
	}
	found, err := s.DiagnosticsRead(DiagnosticsRequest{Source: "gdbserver_stderr", Contains: "vMustReplyEmpty"})
	if err != nil {
		t.Fatal(err)
	}
	matches := found["items"].([]map[string]any)
	if len(matches) != 1 || !strings.Contains(matches[0]["text"].(string), "vMustReplyEmpty") {
		t.Fatalf("bad stderr contains result: %#v", found)
	}
	closeOnce(s.done)
}

func TestRSPBridgeReadyBeforeBusyReplyDoesNotHang(t *testing.T) {
	bp := backpressureErr()
	var m *Manager
	fhc := &fakeHC2{acceptedCh: make(chan []byte, 2)}
	fhc.stdinPlan = func(req int, ref uint64, _ []byte) error {
		if req == 1 {
			if !m.ExecStdinReady(ref) {
				panic("ready not claimed")
			}
			return bp
		}
		return nil
	}
	m, s := newBridgeTestSession(fhc)
	go s.writeTargetStdin()
	if !s.enqueueRSP([]byte("race")) {
		t.Fatal("enqueue failed")
	}
	waitAccepted(t, fhc.acceptedCh, "race")
	fhc.mu.Lock()
	got := string(fhc.stdin[s.ExecRef])
	fhc.mu.Unlock()
	if got != "race" {
		t.Fatalf("target stdin = %q want race", got)
	}
	closeOnce(s.done)
}

func TestRSPBridgeCoalescedReadyEvents(t *testing.T) {
	bp := backpressureErr()
	var m *Manager
	fhc := &fakeHC2{acceptedCh: make(chan []byte, 2)}
	fhc.stdinPlan = func(req int, ref uint64, _ []byte) error {
		if req == 1 {
			_ = m.ExecStdinReady(ref)
			_ = m.ExecStdinReady(ref)
			_ = m.ExecStdinReady(ref)
			return bp
		}
		return nil
	}
	m, s := newBridgeTestSession(fhc)
	go s.writeTargetStdin()
	if !s.enqueueRSP([]byte("coalesced")) {
		t.Fatal("enqueue failed")
	}
	waitAccepted(t, fhc.acceptedCh, "coalesced")
	fhc.mu.Lock()
	got := string(fhc.stdin[s.ExecRef])
	fhc.mu.Unlock()
	if got != "coalesced" {
		t.Fatalf("target stdin = %q want coalesced", got)
	}
	closeOnce(s.done)
}

func TestRSPBridgeBusyThenExecTerminal(t *testing.T) {
	fhc := &fakeHC2{stdinErrs: []error{backpressureErr()}, backpressureCh: make(chan struct{}, 1)}
	m, s := newBridgeTestSession(fhc)
	go s.writeTargetStdin()
	if !s.enqueueRSP([]byte("terminal")) {
		t.Fatal("enqueue failed")
	}
	waitBackpressure(t, fhc.backpressureCh)
	if !m.ExecState(hc2.ExecState{Ref: s.ExecRef, State: 4, ExitCode: 9}) {
		t.Fatal("terminal state was not claimed")
	}
	waitSessionState(t, s, "LOST")
	if msg := fmt.Sprint(s.Status()["last_error"]); !strings.Contains(msg, "GDBSERVER_EXITED") {
		t.Fatalf("expected terminal root cause, got %s", msg)
	}
	closeOnce(s.done)
}

func TestRSPBridgeRepeatedBackpressureCycles(t *testing.T) {
	const cycles = 2000
	bp := backpressureErr()
	fhc := &fakeHC2{backpressureCh: make(chan struct{}, cycles+8)}
	fhc.stdinPlan = func(req int, _ uint64, _ []byte) error {
		if req%2 == 1 {
			return bp
		}
		return nil
	}
	m, s := newBridgeTestSession(fhc)
	go s.writeTargetStdin()
	go func() {
		for range fhc.backpressureCh {
			_ = m.ExecStdinReady(s.ExecRef)
		}
	}()
	payload := make([]byte, cycles*MaxRSPChunk)
	for i := range payload {
		payload[i] = byte(i * 7)
	}
	done := make(chan bool, 1)
	go func() { done <- s.enqueueRSP(payload) }()
	select {
	case ok := <-done:
		if !ok {
			t.Fatal("enqueue aborted")
		}
	case <-time.After(5 * time.Second):
		t.Fatal("repeated backpressure enqueue timed out")
	}
	waitUntil(t, func() bool {
		fhc.mu.Lock()
		defer fhc.mu.Unlock()
		return len(fhc.stdin[s.ExecRef]) == len(payload)
	}, "repeated backpressure delivery")
	fhc.mu.Lock()
	got := append([]byte(nil), fhc.stdin[s.ExecRef]...)
	fhc.mu.Unlock()
	if string(got) != string(payload) {
		t.Fatal("repeated backpressure changed bytes")
	}
	closeOnce(s.done)
}

func TestRSPBridgePendingBoundPausesLocalReads(t *testing.T) {
	accepting := false
	bp := backpressureErr()
	fhc := &fakeHC2{backpressureCh: make(chan struct{}, 32)}
	fhc.stdinPlan = func(int, uint64, []byte) error {
		if accepting {
			return nil
		}
		return bp
	}
	_, s := newBridgeTestSession(fhc)
	go s.writeTargetStdin()

	payload := make([]byte, MaxRSPPendingBytes+MaxRSPChunk*2)
	for i := range payload {
		payload[i] = byte(i)
	}
	done := make(chan bool, 1)
	go func() { done <- s.enqueueRSP(payload) }()
	waitBackpressure(t, fhc.backpressureCh)
	waitUntil(t, func() bool {
		s.rspMu.Lock()
		defer s.rspMu.Unlock()
		return len(s.rspPending) == MaxRSPPendingBytes
	}, "pending buffer to reach explicit bound")
	select {
	case <-done:
		t.Fatal("enqueue completed while target stdin was blocked")
	default:
	}

	fhc.mu.Lock()
	accepting = true
	fhc.mu.Unlock()
	s.notifyStdinReady()
	select {
	case ok := <-done:
		if !ok {
			t.Fatal("enqueue aborted")
		}
	case <-time.After(3 * time.Second):
		t.Fatal("enqueue did not resume after target capacity returned")
	}
	waitUntil(t, func() bool {
		fhc.mu.Lock()
		defer fhc.mu.Unlock()
		return len(fhc.stdin[s.ExecRef]) == len(payload)
	}, "all queued bytes to reach target stdin")
	fhc.mu.Lock()
	got := append([]byte(nil), fhc.stdin[s.ExecRef]...)
	fhc.mu.Unlock()
	if string(got) != string(payload) {
		t.Fatal("bounded bridge changed queued bytes")
	}
	closeOnce(s.done)
}

func TestRSPBridgeFullDuplexBackpressureStress(t *testing.T) {
	bp := backpressureErr()
	fhc := &fakeHC2{backpressureCh: make(chan struct{}, 4096)}
	fhc.stdinPlan = func(req int, _ uint64, _ []byte) error {
		if req%3 == 0 {
			return bp
		}
		return nil
	}
	m, s := newBridgeTestSession(fhc)
	go s.writeTargetStdin()

	c1, c2 := net.Pipe()
	defer c2.Close()
	s.mu.Lock()
	s.conn = c1
	s.mu.Unlock()
	closeOnce(s.connReady)
	go s.writeToGDB()
	go func() {
		for range fhc.backpressureCh {
			_ = m.ExecStdinReady(s.ExecRef)
		}
	}()

	toTarget := make([]byte, 6*MaxRSPPendingBytes+123)
	toGDB := make([]byte, 6*MaxRSPPendingBytes+321)
	for i := range toTarget {
		toTarget[i] = byte(i * 13)
	}
	for i := range toGDB {
		toGDB[i] = byte(i*17 + 3)
	}

	readDone := make(chan []byte, 1)
	go func() {
		buf := make([]byte, len(toGDB))
		_, _ = io.ReadFull(c2, buf)
		readDone <- buf
	}()
	writeDone := make(chan bool, 1)
	go func() { writeDone <- s.enqueueRSP(toTarget) }()
	for off := 0; off < len(toGDB); off += MaxRSPChunk {
		end := off + MaxRSPChunk
		if end > len(toGDB) {
			end = len(toGDB)
		}
		if !m.ExecOutput("stdout", s.ExecRef, toGDB[off:end]) {
			t.Fatal("debug stdout was not claimed")
		}
	}

	select {
	case ok := <-writeDone:
		if !ok {
			t.Fatal("gdb->target enqueue aborted")
		}
	case <-time.After(5 * time.Second):
		t.Fatal("gdb->target stress timed out")
	}
	waitUntil(t, func() bool {
		fhc.mu.Lock()
		defer fhc.mu.Unlock()
		return len(fhc.stdin[s.ExecRef]) == len(toTarget)
	}, "full-duplex target stdin bytes")
	select {
	case got := <-readDone:
		if string(got) != string(toGDB) {
			t.Fatal("target->gdb bytes changed")
		}
	case <-time.After(5 * time.Second):
		t.Fatal("target->gdb stress timed out")
	}
	fhc.mu.Lock()
	gotTarget := append([]byte(nil), fhc.stdin[s.ExecRef]...)
	fhc.mu.Unlock()
	if string(gotTarget) != string(toTarget) {
		t.Fatal("gdb->target bytes changed")
	}
	closeOnce(s.done)
	_ = c1.Close()
}

func TestDebugStartHeavyRSPBackpressureSucceeds(t *testing.T) {
	t.Setenv("LMUX_DEBUG_FAKE_GDB", "1")
	t.Setenv("LMUX_DEBUG_FAKE_GDB_RSP_BYTES", strconv.Itoa(MaxRSPPendingBytes+4096))
	dir := t.TempDir()
	sym := writeSymbols(t, dir)
	bp := backpressureErr()
	fhc := &fakeHC2{backpressureCh: make(chan struct{}, 4096)}
	fhc.stdinPlan = func(req int, _ uint64, _ []byte) error {
		if req%4 == 0 {
			return bp
		}
		return nil
	}
	m := NewManager(Config{RuntimeDir: dir, GDBPath: os.Args[0], TargetGDBServer: "/gdbserver", MaxSessions: 2}, fhc, nil)
	go func() {
		for range fhc.backpressureCh {
			_ = m.ExecStdinReady(1)
		}
	}()
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	s, err := m.Start(ctx, StartRequest{Mode: "launch", Symbols: sym, Program: "/bin/app"})
	if err != nil {
		t.Fatal(err)
	}
	waitUntil(t, func() bool {
		fhc.mu.Lock()
		defer fhc.mu.Unlock()
		return len(fhc.stdin[s.ExecRef]) == MaxRSPPendingBytes+4096
	}, "heavy debug_start RSP bytes")
	if _, err := s.Detach(ctx); err != nil {
		t.Fatal(err)
	}
}

func assertDebugClean(t *testing.T, m *Manager, dir string) {
	t.Helper()
	if m.ActiveCount() != 0 {
		t.Fatalf("debug session leaked")
	}
	m.mu.Lock()
	refCount := len(m.byRef)
	m.mu.Unlock()
	if refCount != 0 {
		t.Fatalf("exec ownership leaked")
	}
	if entries, _ := os.ReadDir(filepath.Join(dir, "debug")); len(entries) != 0 {
		t.Fatalf("socket path leaked: %v", entries)
	}
}

func waitDebugCleaned(t *testing.T, m *Manager) {
	t.Helper()
	waitUntil(t, func() bool {
		m.mu.Lock()
		defer m.mu.Unlock()
		return m.activeCountLocked() == 0 && len(m.byRef) == 0
	}, "debug cleanup to free active capacity")
}

func assertTombstone(t *testing.T, m *Manager, token string) {
	t.Helper()
	s, ok := m.Get(token)
	if !ok {
		t.Fatalf("expected diagnostic tombstone for %s", token)
	}
	st := s.Status()
	if st["state"] != "ERROR" && st["state"] != "LOST" {
		t.Fatalf("expected terminal tombstone, got %#v", st)
	}
	if st["last_error"] == "" {
		t.Fatalf("tombstone lost root cause: %#v", st)
	}
}

func assertReleased(t *testing.T, fhc *fakeHC2, ref uint64) {
	t.Helper()
	assertReleasedOnly(t, fhc, ref)
	fhc.mu.Lock()
	defer fhc.mu.Unlock()
	if len(fhc.signals) < 2 || fhc.signals[0] != hc2.SigTERM || fhc.signals[1] != hc2.SigKILL {
		t.Fatalf("expected TERM/KILL cleanup signals, got %v", fhc.signals)
	}
}

func assertReleasedOnly(t *testing.T, fhc *fakeHC2, ref uint64) {
	t.Helper()
	waitUntil(t, func() bool {
		fhc.mu.Lock()
		defer fhc.mu.Unlock()
		for _, r := range fhc.releases {
			if r == ref {
				return true
			}
		}
		return false
	}, "target gdbserver release")
}

func newBridgeTestSession(fhc *fakeHC2) (*Manager, *Session) {
	m := NewManager(Config{}, fhc, nil)
	s := &Session{
		mgr:        m,
		Token:      "d-test",
		State:      "CONNECTED",
		ExecRef:    1,
		toGDB:      make(chan []byte, MaxRSPQueueChunks),
		connReady:  make(chan struct{}),
		done:       make(chan struct{}),
		execDone:   make(chan struct{}),
		wait:       make(chan struct{}),
		rspData:    make(chan struct{}, 1),
		rspSpace:   make(chan struct{}, 1),
		stdinReady: make(chan struct{}, 1),
	}
	m.sessions[s.Token] = s
	m.byRef[s.ExecRef] = s
	return m, s
}

func backpressureErr() error {
	return &hc2.Error{Code: hc2.ErrBusy, Message: "stdin backpressure"}
}

func waitAccepted(t *testing.T, ch <-chan []byte, want string) {
	t.Helper()
	select {
	case got := <-ch:
		if string(got) != want {
			t.Fatalf("accepted chunk = %q want %q", string(got), want)
		}
	case <-time.After(2 * time.Second):
		t.Fatalf("timed out waiting for accepted chunk %q", want)
	}
}

func waitBackpressure(t *testing.T, ch <-chan struct{}) {
	t.Helper()
	select {
	case <-ch:
	case <-time.After(2 * time.Second):
		t.Fatal("timed out waiting for stdin backpressure")
	}
}

func waitUntil(t *testing.T, pred func() bool, what string) {
	t.Helper()
	deadline := time.Now().Add(6 * time.Second)
	for time.Now().Before(deadline) {
		if pred() {
			return
		}
		time.Sleep(time.Millisecond)
	}
	t.Fatalf("timed out waiting for %s", what)
}

func commandLogContains(path, needle string) bool {
	b, err := os.ReadFile(path)
	return err == nil && strings.Contains(string(b), needle)
}

func mustReadString(t *testing.T, path string) string {
	t.Helper()
	b, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	return string(b)
}

type readResult struct {
	data []byte
	err  error
}

func readPipeExactAsync(r io.Reader, n int) <-chan readResult {
	ch := make(chan readResult, 1)
	go func() {
		buf := make([]byte, n)
		_, err := io.ReadFull(r, buf)
		ch <- readResult{data: buf, err: err}
	}()
	return ch
}

type partialWriteConn struct {
	mu     sync.Mutex
	max    int
	buf    bytes.Buffer
	closed bool
}

func (c *partialWriteConn) Read(_ []byte) (int, error) {
	return 0, io.ErrClosedPipe
}

func (c *partialWriteConn) Write(p []byte) (int, error) {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.closed {
		return 0, io.ErrClosedPipe
	}
	n := c.max
	if n <= 0 || n > len(p) {
		n = len(p)
	}
	c.buf.Write(p[:n])
	return n, nil
}

func (c *partialWriteConn) Close() error {
	c.mu.Lock()
	defer c.mu.Unlock()
	c.closed = true
	return nil
}

func (c *partialWriteConn) LocalAddr() net.Addr              { return nil }
func (c *partialWriteConn) RemoteAddr() net.Addr             { return nil }
func (c *partialWriteConn) SetDeadline(time.Time) error      { return nil }
func (c *partialWriteConn) SetReadDeadline(time.Time) error  { return nil }
func (c *partialWriteConn) SetWriteDeadline(time.Time) error { return nil }

func (c *partialWriteConn) bytes() []byte {
	c.mu.Lock()
	defer c.mu.Unlock()
	return append([]byte(nil), c.buf.Bytes()...)
}

func waitReadExact(t *testing.T, ch <-chan readResult) []byte {
	t.Helper()
	select {
	case res := <-ch:
		if res.err != nil {
			t.Fatalf("read failed: %v", res.err)
		}
		return res.data
	case <-time.After(5 * time.Second):
		t.Fatal("timed out waiting for exact pipe bytes")
		return nil
	}
}

func minInt(a, b int) int {
	if a < b {
		return a
	}
	return b
}

func waitSessionState(t *testing.T, s *Session, want string) map[string]any {
	t.Helper()
	deadline := time.Now().Add(5 * time.Second)
	var st map[string]any
	for time.Now().Before(deadline) {
		st = s.Status()
		if st["state"] == want {
			return st
		}
		s.Wait(context.Background(), 50*time.Millisecond)
	}
	t.Fatalf("timed out waiting for session state %s, last=%#v", want, st)
	return nil
}

func assertSocketPath(t *testing.T, path string) {
	t.Helper()
	st, err := os.Lstat(path)
	if err != nil {
		t.Fatalf("socket path missing: %s: %v", path, err)
	}
	if st.Mode()&os.ModeSocket == 0 {
		t.Fatalf("path is not a unix socket: %s mode=%s", path, st.Mode())
	}
}

func writeSymbols(t *testing.T, dir string) string {
	t.Helper()
	sym := dir + "/a.out"
	if err := os.WriteFile(sym, []byte("symbols"), 0600); err != nil {
		t.Fatal(err)
	}
	return sym
}

func waitForStdin(t *testing.T, fhc *fakeHC2, ref uint64, want string) bool {
	t.Helper()
	deadline := time.Now().Add(time.Second)
	for time.Now().Before(deadline) {
		fhc.mu.Lock()
		got := string(fhc.stdin[ref])
		fhc.mu.Unlock()
		if strings.Contains(got, want) {
			return true
		}
		time.Sleep(time.Millisecond)
	}
	return false
}

func execStatePayload(ref, id uint64, state byte) []byte {
	p := make([]byte, 32)
	binary.LittleEndian.PutUint64(p[:8], ref)
	binary.LittleEndian.PutUint64(p[8:16], id)
	p[16] = state
	return p
}

func okPayload() []byte {
	p := make([]byte, 4)
	binary.LittleEndian.PutUint32(p, hc2.ErrOK)
	return p
}

func decodeExecStartPayload(p []byte) (string, []string, bool) {
	off := 0
	readString := func() (string, bool) {
		if off+2 > len(p) {
			return "", false
		}
		n := int(binary.LittleEndian.Uint16(p[off : off+2]))
		off += 2
		if off+n > len(p) {
			return "", false
		}
		s := string(p[off : off+n])
		off += n
		return s, true
	}
	cwd, ok := readString()
	if !ok || off+2 > len(p) {
		return "", nil, false
	}
	argc := int(binary.LittleEndian.Uint16(p[off : off+2]))
	off += 2
	argv := make([]string, 0, argc)
	for i := 0; i < argc; i++ {
		arg, ok := readString()
		if !ok {
			return "", nil, false
		}
		argv = append(argv, arg)
	}
	return cwd, argv, off == len(p)
}
