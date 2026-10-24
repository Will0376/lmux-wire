package main

import (
	"bufio"
	"context"
	"encoding/binary"
	"fmt"
	"net"
	"os"
	"strings"
	"sync"
	"testing"
	"time"

	debugmgr "lmux/internal/debug"
	"lmux/internal/hc2"

	"github.com/modelcontextprotocol/go-sdk/mcp"
)

func TestMain(m *testing.M) {
	if os.Getenv("LMUX_MCP_FAKE_GDB") == "1" {
		fakeMCPGDB()
		return
	}
	os.Exit(m.Run())
}

func fakeMCPGDB() {
	sc := bufio.NewScanner(os.Stdin)
	var rsp net.Conn
	fmt.Println(`=thread-group-added,id="i1"`)
	fmt.Println(`(gdb) `)
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
		case strings.HasPrefix(cmd, "-interpreter-exec console"):
			fmt.Printf("%s^done\n", tok)
			fmt.Println(`(gdb)`)
		case strings.HasPrefix(cmd, "-gdb-set"):
			fmt.Printf("%s^done\n", tok)
			fmt.Println(`(gdb)`)
		case strings.HasPrefix(cmd, "-target-select"):
			parts := strings.Split(cmd, " ")
			path := strings.Trim(parts[len(parts)-1], `"`)
			if st, err := os.Lstat(path); err != nil || st.Mode()&os.ModeSocket == 0 {
				fmt.Printf(`%s^error,msg="RSP_SOCKET_MISSING"`+"\n", tok)
				continue
			}
			if c, err := net.Dial("unix", path); err == nil {
				rsp = c
				_, _ = rsp.Write([]byte("mcp-gdb-to-target"))
			}
			fmt.Printf("%s^connected\n", tok)
			fmt.Println(`(gdb) `)
			fmt.Println(`*stopped,reason="signal-received",thread-id="1",frame={level="0",addr="0x1000",func="_start"}`)
		case strings.HasPrefix(cmd, "-break-insert"):
			fmt.Printf(`%s^done,bkpt={number="1",addr="0x1010",func="foo",file="foo.c",line="7"}`+"\n", tok)
			fmt.Println(`(gdb)`)
		case strings.HasPrefix(cmd, "-break-delete"):
			fmt.Printf("%s^done\n", tok)
			fmt.Println(`(gdb)`)
		case strings.HasPrefix(cmd, "-exec-continue"):
			fmt.Printf("%s^running\n", tok)
			fmt.Println(`(gdb)`)
			fmt.Println(`*running,thread-id="all"`)
			fmt.Println(`*stopped,reason="breakpoint-hit",thread-id="1",frame={level="0",addr="0x1010",func="foo",fullname="/tmp/foo.c",line="7"}`)
		case strings.HasPrefix(cmd, "-exec-interrupt"):
			fmt.Printf("%s^done\n", tok)
			fmt.Println(`(gdb)`)
			fmt.Println(`*stopped,reason="signal-received",thread-id="1"`)
		case strings.HasPrefix(cmd, "-exec-next-instruction"), strings.HasPrefix(cmd, "-exec-step-instruction"), strings.HasPrefix(cmd, "-exec-next"), strings.HasPrefix(cmd, "-exec-step"), strings.HasPrefix(cmd, "-exec-finish"):
			fmt.Printf("%s^running\n", tok)
			fmt.Println(`(gdb)`)
			fmt.Println(`*running,thread-id="all"`)
			fmt.Println(`*stopped,reason="end-stepping-range",thread-id="1",frame={level="0",addr="0x1020",func="bar",line="9"}`)
		case strings.HasPrefix(cmd, "-stack-list-frames"):
			fmt.Printf(`%s^done,stack=[frame={level="0",addr="0x1020",func="bar",file="bar.c",fullname="/tmp/bar.c",line="9"}]`+"\n", tok)
			fmt.Println(`(gdb)`)
		case strings.HasPrefix(cmd, "-data-evaluate-expression"):
			fmt.Printf(`%s^done,value="42"`+"\n", tok)
			fmt.Println(`(gdb)`)
		case strings.HasPrefix(cmd, "-thread-info"):
			fmt.Printf(`%s^done,threads=[{id="1",target-id="Thread 1"}]`+"\n", tok)
			fmt.Println(`(gdb)`)
		case strings.HasPrefix(cmd, "-thread-select"):
			fmt.Printf("%s^done\n", tok)
			fmt.Println(`(gdb)`)
		case strings.HasPrefix(cmd, "-data-list-register-values"):
			fmt.Printf(`%s^done,register-values=[]`+"\n", tok)
			fmt.Println(`(gdb)`)
		case strings.HasPrefix(cmd, "-data-read-memory-bytes"):
			fmt.Printf(`%s^done,memory=[{begin="0x1000",contents="4142"}]`+"\n", tok)
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
			fmt.Println(`(gdb)`)
		}
	}
}

type mcpFakeHC2 struct {
	mu      sync.Mutex
	nextRef uint64
	stdin   map[uint64][]byte
}

func (f *mcpFakeHC2) Request(ctx context.Context, typ uint16, payload []byte) (hc2.Frame, error) {
	f.mu.Lock()
	defer f.mu.Unlock()
	switch typ {
	case hc2.TypeExecStart:
		f.nextRef++
		return hc2.Frame{Type: hc2.TypeExecStarted, Payload: mcpExecStatePayload(f.nextRef, 2)}, nil
	case hc2.TypeExecStdinData:
		if f.stdin == nil {
			f.stdin = map[uint64][]byte{}
		}
		ref := binary.LittleEndian.Uint64(payload[:8])
		f.stdin[ref] = append(f.stdin[ref], payload[8:]...)
		return hc2.Frame{Type: hc2.TypeError, Payload: mcpOKPayload()}, nil
	case hc2.TypeExecSignal, hc2.TypeExecRelease:
		return hc2.Frame{Type: hc2.TypeError, Payload: mcpOKPayload()}, nil
	default:
		return hc2.Frame{Type: hc2.TypeError, Payload: mcpOKPayload()}, nil
	}
}

func TestMCPDebugToolLifecycleWithFakeGDB(t *testing.T) {
	t.Setenv("LMUX_MCP_FAKE_GDB", "1")
	a := testApp(t)
	fhc := &mcpFakeHC2{}
	a.debug = debugmgr.NewManager(debugmgr.Config{
		RuntimeDir:      t.TempDir(),
		GDBPath:         os.Args[0],
		TargetGDBServer: "/target/gdbserver",
		MaxSessions:     2,
	}, fhc, a.session.Journal)
	server := newMCPServer(a)
	clientSession := connectInMemory(t, server)
	defer clientSession.Close()
	caps := callTool(t, clientSession, "debug_capabilities", map[string]any{})
	if caps["gdb_remote_timeout_seconds"] != float64(debugmgr.DefaultGDBRemoteTimeoutSeconds) {
		t.Fatalf("debug capabilities missing remote timeout: %#v", caps)
	}
	if caps["mi_async"] != true {
		t.Fatalf("debug capabilities missing mi_async: %#v", caps)
	}
	if caps["interrupt_transport"] != "gdb-rsp" || caps["interrupt_stop_requires_wait"] != true {
		t.Fatalf("debug capabilities missing interrupt semantics: %#v", caps)
	}

	sym := a.manager.RuntimeDir + "/symbols.elf"
	if err := os.WriteFile(sym, []byte("symbols"), 0600); err != nil {
		t.Fatal(err)
	}
	start := callTool(t, clientSession, "debug_start", map[string]any{"mode": "launch", "symbols": sym, "program": "/bin/app"})
	token, _ := start["debug_token"].(string)
	if token == "" || start["state"] == "ERROR" {
		t.Fatalf("bad debug_start: %v", start)
	}
	waitForMCPStdin(t, fhc, "mcp-gdb-to-target")
	diag := callTool(t, clientSession, "debug_diagnostics_read", map[string]any{"debug_token": token, "source": "rsp", "contains": "mcp-gdb-to-target"})
	if items, _ := diag["items"].([]any); len(items) != 1 {
		t.Fatalf("bad debug diagnostics read: %v", diag)
	}
	status := callTool(t, clientSession, "debug_status", map[string]any{"debug_token": token})
	if status["state"] != "STOPPED" {
		t.Fatalf("bad status: %v", status)
	}
	if status["gdb_remote_timeout_seconds"] != float64(debugmgr.DefaultGDBRemoteTimeoutSeconds) {
		t.Fatalf("debug_status missing remote timeout: %#v", status)
	}
	if status["mi_async"] != true {
		t.Fatalf("debug_status missing mi_async: %#v", status)
	}
	if _, exists := status["rsp_trace"]; exists {
		t.Fatalf("debug_status should not return full rsp_trace: %v", status)
	}
	bp := callTool(t, clientSession, "debug_breakpoint_set", map[string]any{"debug_token": token, "location": "foo"})
	if bp["breakpoint_id"] != "1" {
		t.Fatalf("bad breakpoint: %v", bp)
	}
	callTool(t, clientSession, "debug_continue", map[string]any{"debug_token": token})
	waitForMCPDebugStopped(t, clientSession, token, "breakpoint-hit", time.Second)
	callTool(t, clientSession, "debug_interrupt", map[string]any{"debug_token": token})
	callTool(t, clientSession, "debug_step", map[string]any{"debug_token": token, "kind": "next"})
	bt := callTool(t, clientSession, "debug_backtrace", map[string]any{"debug_token": token, "max_frames": 8})
	if frames, _ := bt["frames"].([]any); len(frames) != 1 {
		t.Fatalf("bad backtrace: %v", bt)
	}
	ev := callTool(t, clientSession, "debug_evaluate", map[string]any{"debug_token": token, "expression": "x"})
	if ev["value"] != "42" {
		t.Fatalf("bad evaluate: %v", ev)
	}
	callTool(t, clientSession, "debug_threads", map[string]any{"debug_token": token})
	callTool(t, clientSession, "debug_select_thread", map[string]any{"debug_token": token, "thread_id": "1"})
	callTool(t, clientSession, "debug_registers", map[string]any{"debug_token": token})
	mem := callTool(t, clientSession, "debug_memory_read", map[string]any{"debug_token": token, "address": "0x1000", "length": 2})
	if mem["bytes"].(float64) != 2 || mem["base64"] != "QUI=" || len(mem) != 2 {
		t.Fatalf("bad memory read: %v", mem)
	}
	hexMem := callTool(t, clientSession, "debug_memory_read", map[string]any{"debug_token": token, "address": "symbol+4", "length": 2, "format": "hex"})
	if hexMem["address"] != "0x1000" || hexMem["bytes"].(float64) != 2 ||
		!strings.Contains(hexMem["hex"].(string), "41 42") ||
		!strings.HasSuffix(hexMem["hex"].(string), "|AB|") {
		t.Fatalf("bad hex memory read: %v", hexMem)
	}
	bothMem := callTool(t, clientSession, "debug_memory_read", map[string]any{"debug_token": token, "address": "0x1000", "length": 2, "format": "both"})
	if bothMem["base64"] != "QUI=" || bothMem["hex"] != hexMem["hex"] {
		t.Fatalf("bad both memory read: %v", bothMem)
	}
	invalidMem := callToolRaw(t, clientSession, 5*time.Second, "debug_memory_read", map[string]any{"debug_token": token, "address": "0x1000", "length": 2, "format": "raw"})
	if !invalidMem.IsError {
		t.Fatalf("invalid memory format accepted: %#v", invalidMem.StructuredContent)
	}
	callTool(t, clientSession, "debug_breakpoint_delete", map[string]any{"debug_token": token, "breakpoint_id": "1"})
	detach := callTool(t, clientSession, "debug_detach", map[string]any{"debug_token": token})
	if detach["detached"] != true {
		t.Fatalf("bad detach: %v", detach)
	}
}

func TestMCPDebugStartRemoteDebugModes(t *testing.T) {
	t.Setenv("LMUX_MCP_FAKE_GDB", "1")
	a := testApp(t)
	fhc := &mcpFakeHC2{}
	a.debug = debugmgr.NewManager(debugmgr.Config{
		RuntimeDir:      t.TempDir(),
		GDBPath:         os.Args[0],
		TargetGDBServer: "/target/gdbserver",
		MaxSessions:     2,
	}, fhc, a.session.Journal)
	server := newMCPServer(a)
	clientSession := connectInMemory(t, server)
	defer clientSession.Close()

	sym := a.manager.RuntimeDir + "/symbols.elf"
	if err := os.WriteFile(sym, []byte("symbols"), 0600); err != nil {
		t.Fatal(err)
	}
	for _, mode := range []string{"off", "gdbserver", "gdb", "both"} {
		start := callTool(t, clientSession, "debug_start", map[string]any{"mode": "launch", "symbols": sym, "program": "/bin/app", "remote_debug": mode})
		if start["remote_debug"] != mode {
			t.Fatalf("remote_debug=%#v want %q start=%#v", start["remote_debug"], mode, start)
		}
		callTool(t, clientSession, "debug_stop", map[string]any{"debug_token": start["debug_token"]})
	}

	res, err := clientSession.CallTool(context.Background(), &mcp.CallToolParams{
		Name:      "debug_start",
		Arguments: map[string]any{"mode": "launch", "symbols": sym, "program": "/bin/app", "remote_debug": "bad"},
	})
	if err != nil {
		t.Fatal(err)
	}
	if !res.IsError || !strings.Contains(toolText(res), "remote_debug") {
		t.Fatalf("invalid remote_debug should be rejected, result=%#v text=%q", res, toolText(res))
	}
}

func waitForMCPStdin(t *testing.T, fhc *mcpFakeHC2, want string) {
	t.Helper()
	deadline := time.Now().Add(time.Second)
	for time.Now().Before(deadline) {
		fhc.mu.Lock()
		for _, data := range fhc.stdin {
			if strings.Contains(string(data), want) {
				fhc.mu.Unlock()
				return
			}
		}
		fhc.mu.Unlock()
		time.Sleep(time.Millisecond)
	}
	t.Fatalf("target stdin missing %q", want)
}

func waitForMCPDebugStopped(t *testing.T, session *mcp.ClientSession,
	token, reason string, timeout time.Duration) map[string]any {
	t.Helper()
	deadline := time.Now().Add(timeout)
	var last map[string]any
	for {
		remaining := time.Until(deadline)
		if remaining <= 0 {
			t.Fatalf("timed out waiting for debug stop reason %q, last=%v", reason, last)
		}
		timeoutMS := int(remaining / time.Millisecond)
		if timeoutMS < 1 {
			timeoutMS = 1
		}
		last = callTool(t, session, "debug_wait", map[string]any{
			"debug_token": token,
			"timeout_ms":  timeoutMS,
		})
		switch last["state"] {
		case "STOPPED":
			if last["stop_reason"] != reason {
				t.Fatalf("debug stopped for unexpected reason: %v", last)
			}
			return last
		case "RUNNING":
			// debug_wait reports every state notification. A queued *running
			// event may therefore wake this waiter before the following
			// *stopped event is published.
		default:
			t.Fatalf("unexpected debug state while waiting for stop: %v", last)
		}
	}
}

func mcpExecStatePayload(ref uint64, state byte) []byte {
	p := make([]byte, 32)
	binary.LittleEndian.PutUint64(p[:8], ref)
	binary.LittleEndian.PutUint64(p[8:16], 100+ref)
	p[16] = state
	return p
}

func mcpOKPayload() []byte {
	p := make([]byte, 4)
	binary.LittleEndian.PutUint32(p, hc2.ErrOK)
	return p
}
