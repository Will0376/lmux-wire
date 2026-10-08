package main

import (
	"context"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"os"
	"strconv"
	"strings"
	"sync"
	"time"

	debugmgr "lmux/internal/debug"
	"lmux/internal/hc2"
	"lmux/internal/mcpapp"

	"github.com/modelcontextprotocol/go-sdk/mcp"
)

type app struct {
	controlPath string
	manager     *mcpapp.Manager
	session     *mcpapp.Session
	debug       *debugmgr.Manager
	debugConfig debugmgr.Config

	mu       sync.Mutex
	hc       *hc2.Client
	caps     uint32
	execRefs map[uint64]string
	fileWait map[uint64]chan fileResult
	fileKind map[uint64]string
}

type fileResult struct {
	done hc2.FileDone
	err  hc2.FileError
}

func main() {
	var controlPath, runtimeDir string
	var gdbPath, targetGDBServer, gdbSysroot, gdbSolib string
	var maxDebugSessions, gdbRemoteTimeout int
	flag.StringVar(&controlPath, "control", envDefault("LMUX_MCP_CONTROL", "/run/lmux/control-v2.sock"), "lmuxd structured control socket")
	flag.StringVar(&runtimeDir, "runtime-dir", envDefault("LMUX_MCP_RUNTIME_DIR", ""), "lmux-mcp runtime directory (default: $XDG_RUNTIME_DIR/lmux/mcp)")
	flag.StringVar(&runtimeDir, "runtime", envDefault("LMUX_MCP_RUNTIME_DIR", ""), "alias for --runtime-dir")
	flag.StringVar(&gdbPath, "gdb", envDefault("LMUX_MCP_GDB", ""), "host GDB executable for debug tools")
	flag.StringVar(&targetGDBServer, "target-gdbserver", envDefault("LMUX_MCP_TARGET_GDBSERVER", ""), "absolute target gdbserver path for debug tools")
	flag.IntVar(&maxDebugSessions, "max-debug-sessions", 2, "maximum active debug sessions")
	flag.StringVar(&gdbSysroot, "gdb-sysroot", envDefault("LMUX_MCP_GDB_SYSROOT", ""), "optional GDB sysroot")
	flag.StringVar(&gdbSolib, "gdb-solib-search-path", envDefault("LMUX_MCP_GDB_SOLIB_SEARCH_PATH", ""), "optional GDB solib-search-path")
	flag.IntVar(&gdbRemoteTimeout, "gdb-remote-timeout", envIntDefault("LMUX_MCP_GDB_REMOTE_TIMEOUT", debugmgr.DefaultGDBRemoteTimeoutSeconds), "GDB remotetimeout in seconds for target remote negotiation (1..300)")
	flag.Parse()
	if err := validateGDBRemoteTimeoutSeconds(gdbRemoteTimeout); err != nil {
		fmt.Fprintf(os.Stderr, "lmux-mcp: %v\n", err)
		os.Exit(2)
	}

	a := &app{controlPath: controlPath, manager: mcpapp.NewManager(runtimeDir), execRefs: make(map[uint64]string), fileWait: make(map[uint64]chan fileResult), fileKind: make(map[uint64]string)}
	a.debugConfig = debugmgr.Config{GDBPath: gdbPath, TargetGDBServer: targetGDBServer, MaxSessions: maxDebugSessions, Sysroot: gdbSysroot, SolibSearchPath: gdbSolib, GDBRemoteTimeoutSeconds: gdbRemoteTimeout, TargetExecCap: debugmgr.TargetExecCapacity}
	s, err := a.manager.Start()
	if err != nil {
		fmt.Fprintf(os.Stderr, "lmux-mcp: %v\n", err)
		os.Exit(1)
	}
	a.session = s
	a.debugConfig.RuntimeDir = a.manager.RuntimeDir
	defer a.close()
	if err := a.ensureControl(context.Background()); err != nil {
		s.Journal("ERROR", map[string]any{"message": err.Error()})
	}
	server := newMCPServer(a)
	if err := server.Run(context.Background(), &mcp.StdioTransport{}); err != nil && !errors.Is(err, io.EOF) {
		fmt.Fprintf(os.Stderr, "lmux-mcp: %v\n", err)
	}
	s.Journal("SESSION_DISCONNECT", nil)
}

func (a *app) close() {
	a.mu.Lock()
	debug := a.debug
	a.mu.Unlock()
	if debug != nil {
		ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		debug.StopAll(ctx)
		cancel()
	}
	a.manager.Close()
}

func envDefault(k, d string) string {
	if v := os.Getenv(k); v != "" {
		return v
	}
	return d
}

func envIntDefault(k string, d int) int {
	v := os.Getenv(k)
	if v == "" {
		return d
	}
	n, err := strconv.Atoi(v)
	if err != nil {
		fmt.Fprintf(os.Stderr, "lmux-mcp: invalid %s=%q\n", k, v)
		os.Exit(2)
	}
	return n
}

func validateGDBRemoteTimeoutSeconds(n int) error {
	if n < 1 || n > 300 {
		return fmt.Errorf("--gdb-remote-timeout must be in range 1..300 seconds")
	}
	return nil
}

func (a *app) ensureControl(ctx context.Context) error {
	a.mu.Lock()
	if a.hc != nil {
		a.mu.Unlock()
		return nil
	}
	a.mu.Unlock()
	cctx, cancel := context.WithTimeout(ctx, 5*time.Second)
	defer cancel()
	c, caps, err := hc2.Dial(cctx, a.controlPath, a)
	if err != nil {
		return err
	}
	a.mu.Lock()
	a.hc = c
	a.caps = caps
	if a.debug == nil {
		a.debugConfig.CommandActiveFunc = func() int {
			_, total := a.session.ActiveCommandCounts()
			return total
		}
		a.debug = debugmgr.NewManager(a.debugConfig, c, a.session.Journal)
	}
	a.mu.Unlock()
	a.session.Journal("LMUX_CONNECTED", map[string]any{"control": a.controlPath})
	return nil
}

func (a *app) client() (*hc2.Client, uint32, error) {
	if err := a.ensureControl(context.Background()); err != nil {
		return nil, 0, err
	}
	a.mu.Lock()
	defer a.mu.Unlock()
	return a.hc, a.caps, nil
}

func (a *app) ExecOutput(stream string, ref uint64, data []byte) {
	a.mu.Lock()
	debug := a.debug
	a.mu.Unlock()
	if debug != nil && debug.ExecOutput(stream, ref, data) {
		return
	}
	a.mu.Lock()
	token := a.execRefs[ref]
	a.mu.Unlock()
	if token != "" {
		_ = a.session.AppendOutput(token, stream, data)
	}
}

func (a *app) ExecState(st hc2.ExecState) {
	a.mu.Lock()
	debug := a.debug
	a.mu.Unlock()
	if debug != nil && debug.ExecState(st) {
		return
	}
	a.mu.Lock()
	token := a.execRefs[st.Ref]
	a.mu.Unlock()
	if token != "" {
		_ = a.session.UpdateState(token, st)
	}
}

func (a *app) ExecLost(ref uint64, reason uint32) {
	a.mu.Lock()
	debug := a.debug
	a.mu.Unlock()
	if debug != nil && debug.ExecLost(ref, reason) {
		return
	}
	a.session.MarkLostByRef(ref, reason)
}

func (a *app) ExecStdinReady(ref uint64) {
	a.mu.Lock()
	debug := a.debug
	a.mu.Unlock()
	if debug != nil {
		_ = debug.ExecStdinReady(ref)
	}
}

func (a *app) FileProgress(opID, bytes uint64) {
	a.session.Journal("FILE_PROGRESS", map[string]any{"file_op_id": opID, "bytes": bytes})
}

func (a *app) FileDone(done hc2.FileDone) {
	a.mu.Lock()
	ch := a.fileWait[done.OpID]
	delete(a.fileWait, done.OpID)
	event := a.fileKind[done.OpID]
	delete(a.fileKind, done.OpID)
	a.mu.Unlock()
	if event == "" {
		event = "FILE_DONE"
	}
	a.session.Journal(event, map[string]any{"file_op_id": done.OpID, "bytes": done.Bytes, "sha256": hexString(done.SHA256)})
	if ch != nil {
		ch <- fileResult{done: done}
		close(ch)
	}
}

func (a *app) FileError(err hc2.FileError) {
	a.mu.Lock()
	ch := a.fileWait[err.OpID]
	delete(a.fileWait, err.OpID)
	delete(a.fileKind, err.OpID)
	a.mu.Unlock()
	a.session.Journal("FILE_ERROR", map[string]any{"file_op_id": err.OpID, "code": err.Code, "message": err.Message})
	if ch != nil {
		ch <- fileResult{err: err}
		close(ch)
	}
}

func mustJSON(v any) string {
	b, err := json.MarshalIndent(v, "", "  ")
	if err != nil {
		return fmt.Sprintf("%v", v)
	}
	return string(b)
}

func newMCPServer(a *app) *mcp.Server {
	server := mcp.NewServer(&mcp.Implementation{Name: "lmux-mcp", Version: "0.1"}, nil)
	for _, spec := range tools() {
		name := spec.Name
		server.AddTool(&mcp.Tool{
			Name:        spec.Name,
			Description: spec.Description,
			InputSchema: spec.InputSchema,
		}, func(_ context.Context, req *mcp.CallToolRequest) (*mcp.CallToolResult, error) {
			out, err := a.callTool(name, req.Params.Arguments)
			if err != nil {
				return &mcp.CallToolResult{
					Content: []mcp.Content{&mcp.TextContent{Text: err.Error()}},
					IsError: true,
				}, nil
			}
			return &mcp.CallToolResult{
				Content:           []mcp.Content{&mcp.TextContent{Text: mustJSON(out)}},
				StructuredContent: out,
			}, nil
		})
	}
	return server
}

func (a *app) callTool(name string, args json.RawMessage) (any, error) {
	switch name {
	case "lmux_status":
		return a.toolStatus()
	case "command_start":
		var p struct {
			Argv []string `json:"argv"`
			CWD  string   `json:"cwd"`
			Kind string   `json:"kind"`
		}
		if err := json.Unmarshal(args, &p); err != nil {
			return nil, err
		}
		return a.toolCommandStart(p.Argv, p.CWD, p.Kind)
	case "command_status":
		var p struct {
			CommandToken string `json:"command_token"`
		}
		if err := json.Unmarshal(args, &p); err != nil {
			return nil, err
		}
		return a.toolCommandStatus(p.CommandToken)
	case "command_read":
		var p struct {
			CommandToken string `json:"command_token"`
			Cursor       string `json:"cursor"`
			MaxBytes     int    `json:"max_bytes"`
			MaxEvents    int    `json:"max_events"`
		}
		if err := json.Unmarshal(args, &p); err != nil {
			return nil, err
		}
		return a.toolCommandRead(p.CommandToken, p.Cursor, p.MaxBytes, p.MaxEvents)
	case "command_write_stdin":
		var p struct {
			CommandToken string `json:"command_token"`
			Text         string `json:"text"`
			Base64       string `json:"base64"`
		}
		if err := json.Unmarshal(args, &p); err != nil {
			return nil, err
		}
		return a.toolCommandWriteStdin(p.CommandToken, p.Text, p.Base64)
	case "command_close_stdin":
		var p struct {
			CommandToken string `json:"command_token"`
		}
		if err := json.Unmarshal(args, &p); err != nil {
			return nil, err
		}
		return a.toolCommandCloseStdin(p.CommandToken)
	case "command_signal":
		var p struct {
			CommandToken string `json:"command_token"`
			Signal       string `json:"signal"`
		}
		if err := json.Unmarshal(args, &p); err != nil {
			return nil, err
		}
		return a.toolCommandSignal(p.CommandToken, p.Signal)
	case "command_release":
		var p struct {
			CommandToken string `json:"command_token"`
		}
		if err := json.Unmarshal(args, &p); err != nil {
			return nil, err
		}
		return a.toolCommandRelease(p.CommandToken)
	case "file_put":
		var p struct {
			LocalSource       string `json:"local_source"`
			TargetDestination string `json:"target_destination"`
		}
		if err := json.Unmarshal(args, &p); err != nil {
			return nil, err
		}
		return a.toolFile(hc2.TypeFilePutStart, p.LocalSource, p.TargetDestination)
	case "file_get":
		var p struct {
			TargetSource     string `json:"target_source"`
			LocalDestination string `json:"local_destination"`
		}
		if err := json.Unmarshal(args, &p); err != nil {
			return nil, err
		}
		return a.toolFile(hc2.TypeFileGetStart, p.TargetSource, p.LocalDestination)
	case "debug_capabilities":
		return a.toolDebugCapabilities()
	case "debug_start":
		var p struct {
			Mode        string   `json:"mode"`
			Symbols     string   `json:"symbols"`
			PID         int      `json:"pid"`
			Program     string   `json:"program"`
			Args        []string `json:"args"`
			Label       string   `json:"label"`
			RemoteDebug string   `json:"remote_debug"`
		}
		if err := json.Unmarshal(args, &p); err != nil {
			return nil, err
		}
		return a.toolDebugStart(debugmgr.StartRequest{Mode: p.Mode, Symbols: p.Symbols, PID: p.PID, Program: p.Program, Args: p.Args, Label: p.Label, RemoteDebug: p.RemoteDebug})
	case "debug_list":
		return a.toolDebugList()
	case "debug_status":
		var p struct {
			DebugToken string `json:"debug_token"`
		}
		if err := json.Unmarshal(args, &p); err != nil {
			return nil, err
		}
		return a.toolDebugStatus(p.DebugToken)
	case "debug_diagnostics_read":
		var p struct {
			DebugToken string `json:"debug_token"`
			Source     string `json:"source"`
			Cursor     string `json:"cursor"`
			MaxBytes   int    `json:"max_bytes"`
			MaxEvents  int    `json:"max_events"`
			Contains   string `json:"contains"`
		}
		if err := json.Unmarshal(args, &p); err != nil {
			return nil, err
		}
		return a.toolDebugDiagnosticsRead(p.DebugToken, debugmgr.DiagnosticsRequest{Source: p.Source, Cursor: p.Cursor, MaxBytes: p.MaxBytes, MaxEvents: p.MaxEvents, Contains: p.Contains})
	case "debug_breakpoint_set":
		var p struct {
			DebugToken string `json:"debug_token"`
			Location   string `json:"location"`
			Temporary  bool   `json:"temporary"`
			Condition  string `json:"condition"`
		}
		if err := json.Unmarshal(args, &p); err != nil {
			return nil, err
		}
		return a.withDebug(p.DebugToken, func(s *debugmgr.Session) (any, error) {
			ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
			defer cancel()
			return s.Breakpoint(ctx, p.Location, p.Temporary, p.Condition)
		})
	case "debug_breakpoint_delete":
		var p struct {
			DebugToken string `json:"debug_token"`
			Breakpoint string `json:"breakpoint_id"`
		}
		if err := json.Unmarshal(args, &p); err != nil {
			return nil, err
		}
		return a.withDebug(p.DebugToken, func(s *debugmgr.Session) (any, error) {
			ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
			defer cancel()
			return s.BreakpointDelete(ctx, p.Breakpoint)
		})
	case "debug_continue":
		var p struct {
			DebugToken string `json:"debug_token"`
		}
		if err := json.Unmarshal(args, &p); err != nil {
			return nil, err
		}
		return a.withDebug(p.DebugToken, func(s *debugmgr.Session) (any, error) {
			ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
			defer cancel()
			return s.Continue(ctx)
		})
	case "debug_interrupt":
		var p struct {
			DebugToken string `json:"debug_token"`
		}
		if err := json.Unmarshal(args, &p); err != nil {
			return nil, err
		}
		return a.withDebug(p.DebugToken, func(s *debugmgr.Session) (any, error) {
			ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
			defer cancel()
			return s.Interrupt(ctx)
		})
	case "debug_step":
		var p struct {
			DebugToken string `json:"debug_token"`
			Kind       string `json:"kind"`
		}
		if err := json.Unmarshal(args, &p); err != nil {
			return nil, err
		}
		return a.withDebug(p.DebugToken, func(s *debugmgr.Session) (any, error) {
			ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
			defer cancel()
			return s.Step(ctx, p.Kind)
		})
	case "debug_wait":
		var p struct {
			DebugToken string `json:"debug_token"`
			TimeoutMS  int    `json:"timeout_ms"`
		}
		if err := json.Unmarshal(args, &p); err != nil {
			return nil, err
		}
		return a.withDebug(p.DebugToken, func(s *debugmgr.Session) (any, error) {
			return s.Wait(context.Background(), time.Duration(p.TimeoutMS)*time.Millisecond), nil
		})
	case "debug_threads":
		var p struct {
			DebugToken string `json:"debug_token"`
		}
		if err := json.Unmarshal(args, &p); err != nil {
			return nil, err
		}
		return a.withDebug(p.DebugToken, func(s *debugmgr.Session) (any, error) {
			ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
			defer cancel()
			return s.Threads(ctx)
		})
	case "debug_select_thread":
		var p struct {
			DebugToken string `json:"debug_token"`
			ThreadID   string `json:"thread_id"`
		}
		if err := json.Unmarshal(args, &p); err != nil {
			return nil, err
		}
		return a.withDebug(p.DebugToken, func(s *debugmgr.Session) (any, error) {
			ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
			defer cancel()
			return s.SelectThread(ctx, p.ThreadID)
		})
	case "debug_backtrace":
		var p struct {
			DebugToken string `json:"debug_token"`
			MaxFrames  int    `json:"max_frames"`
		}
		if err := json.Unmarshal(args, &p); err != nil {
			return nil, err
		}
		return a.withDebug(p.DebugToken, func(s *debugmgr.Session) (any, error) {
			ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
			defer cancel()
			return s.Backtrace(ctx, p.MaxFrames)
		})
	case "debug_evaluate":
		var p struct {
			DebugToken string `json:"debug_token"`
			Expression string `json:"expression"`
		}
		if err := json.Unmarshal(args, &p); err != nil {
			return nil, err
		}
		return a.withDebug(p.DebugToken, func(s *debugmgr.Session) (any, error) {
			ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
			defer cancel()
			return s.Evaluate(ctx, p.Expression)
		})
	case "debug_registers":
		var p struct {
			DebugToken string `json:"debug_token"`
		}
		if err := json.Unmarshal(args, &p); err != nil {
			return nil, err
		}
		return a.withDebug(p.DebugToken, func(s *debugmgr.Session) (any, error) {
			ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
			defer cancel()
			return s.Registers(ctx)
		})
	case "debug_memory_read":
		var p struct {
			DebugToken string `json:"debug_token"`
			Address    string `json:"address"`
			Length     int    `json:"length"`
		}
		if err := json.Unmarshal(args, &p); err != nil {
			return nil, err
		}
		return a.withDebug(p.DebugToken, func(s *debugmgr.Session) (any, error) {
			ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
			defer cancel()
			return s.MemoryRead(ctx, p.Address, p.Length)
		})
	case "debug_detach":
		var p struct {
			DebugToken string `json:"debug_token"`
		}
		if err := json.Unmarshal(args, &p); err != nil {
			return nil, err
		}
		return a.withDebug(p.DebugToken, func(s *debugmgr.Session) (any, error) {
			ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
			defer cancel()
			return s.Detach(ctx)
		})
	case "debug_stop":
		var p struct {
			DebugToken string `json:"debug_token"`
		}
		if err := json.Unmarshal(args, &p); err != nil {
			return nil, err
		}
		return a.withDebug(p.DebugToken, func(s *debugmgr.Session) (any, error) {
			ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
			defer cancel()
			return map[string]any{"stopped": true, "debug_token": p.DebugToken}, s.Stop(ctx)
		})
	default:
		return nil, fmt.Errorf("unknown tool %s", name)
	}
}

func (a *app) debugManager() (*debugmgr.Manager, error) {
	a.mu.Lock()
	if a.debug != nil {
		defer a.mu.Unlock()
		return a.debug, nil
	}
	a.mu.Unlock()
	if _, _, err := a.client(); err != nil {
		return nil, err
	}
	a.mu.Lock()
	defer a.mu.Unlock()
	if a.debug == nil {
		return nil, errors.New("debug manager unavailable")
	}
	return a.debug, nil
}

func (a *app) withDebug(token string, fn func(*debugmgr.Session) (any, error)) (any, error) {
	d, err := a.debugManager()
	if err != nil {
		return nil, err
	}
	s, ok := d.Get(token)
	if !ok {
		return nil, errors.New("NOT_FOUND")
	}
	return fn(s)
}

func (a *app) toolStatus() (any, error) {
	c, caps, err := a.client()
	if err != nil {
		return nil, err
	}
	if caps&hc2.CapStatus == 0 {
		return nil, errors.New("STATUS capability unavailable")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	fr, err := c.Request(ctx, hc2.TypeStatusRequest, nil)
	if err != nil {
		return nil, err
	}
	return hc2.DecodeStatus(fr.Payload)
}

func (a *app) toolCommandStart(argv []string, cwd string, kind string) (any, error) {
	if len(argv) == 0 || !strings.HasPrefix(argv[0], "/") {
		return nil, errors.New("argv[0] must be an absolute target path")
	}
	kind, err := mcpapp.NormalizeCommandKind(kind)
	if err != nil {
		return nil, err
	}
	activeSession, activeTotal := a.session.ActiveCommandCounts()
	if kind == "session" && activeSession >= mcpapp.DefaultMaxSessionCommands {
		return nil, errors.New("RESOURCE_LIMIT: max active session commands reached")
	}
	debugActive := 0
	a.mu.Lock()
	if a.debug != nil {
		debugActive = a.debug.ActiveCount()
	}
	a.mu.Unlock()
	if activeTotal+debugActive >= mcpapp.DefaultMaxConcurrentCommand {
		return nil, errors.New("RESOURCE_LIMIT: max concurrent commands reached")
	}
	c, caps, err := a.client()
	if err != nil {
		return nil, err
	}
	if caps&hc2.CapExec == 0 {
		return nil, errors.New("EXEC capability unavailable")
	}
	p, err := hc2.EncodeExecStart(cwd, argv)
	if err != nil {
		return nil, err
	}
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	var fr hc2.Frame
	for {
		fr, err = c.Request(ctx, hc2.TypeExecStart, p)
		if err == nil {
			break
		}
		var hc2Err *hc2.Error
		if errors.As(err, &hc2Err) && hc2Err.Code == hc2.ErrNotConnected {
			select {
			case <-ctx.Done():
				return nil, ctx.Err()
			case <-time.After(20 * time.Millisecond):
				continue
			}
		}
		return nil, err
	}
	if fr.Type != hc2.TypeExecStarted {
		return nil, fmt.Errorf("unexpected response frame %d", fr.Type)
	}
	st, ok := hc2.DecodeExecState(fr.Payload)
	if !ok || st.Ref == 0 {
		return nil, errors.New("bad EXEC_STARTED")
	}
	cmd, err := a.session.NewCommand(argv, cwd, st.Ref, st.TargetExecID, kind)
	if err != nil {
		return nil, err
	}
	a.mu.Lock()
	a.execRefs[st.Ref] = cmd.Token
	a.mu.Unlock()
	_ = a.session.UpdateState(cmd.Token, st)
	return map[string]any{"command_token": cmd.Token, "initial_cursor": a.session.InitialCursor(), "state": cmd.State, "kind": cmd.Kind}, nil
}

func (a *app) toolDebugCapabilities() (any, error) {
	d, err := a.debugManager()
	if err != nil {
		return nil, err
	}
	return d.Capabilities(), nil
}

func (a *app) toolDebugStart(req debugmgr.StartRequest) (any, error) {
	activeSession, activeTotal := a.session.ActiveCommandCounts()
	_ = activeSession
	d, err := a.debugManager()
	if err != nil {
		return nil, err
	}
	if activeTotal+d.ActiveCount() >= mcpapp.DefaultMaxConcurrentCommand {
		return nil, errors.New("RESOURCE_LIMIT")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	s, err := d.Start(ctx, req)
	if err != nil {
		return nil, err
	}
	st := s.Status()
	st["debug_token"] = s.Token
	st["state"] = s.State
	return st, nil
}

func (a *app) toolDebugList() (any, error) {
	d, err := a.debugManager()
	if err != nil {
		return nil, err
	}
	return map[string]any{"sessions": d.List()}, nil
}

func (a *app) toolDebugStatus(token string) (any, error) {
	return a.withDebug(token, func(s *debugmgr.Session) (any, error) {
		return s.Status(), nil
	})
}

func (a *app) toolDebugDiagnosticsRead(token string, req debugmgr.DiagnosticsRequest) (any, error) {
	return a.withDebug(token, func(s *debugmgr.Session) (any, error) {
		out, err := s.DiagnosticsRead(req)
		if err != nil {
			if err.Error() == "CURSOR_EXPIRED" {
				return map[string]any{"error": "CURSOR_EXPIRED"}, nil
			}
			return nil, err
		}
		return out, nil
	})
}

func (a *app) toolCommandStatus(token string) (any, error) {
	cmd, ok := a.session.Command(token)
	if !ok {
		return nil, errors.New("NOT_FOUND")
	}
	if cmd.State != "LOST" && !cmd.Released && cmd.ExecRef != 0 {
		if c, _, err := a.client(); err == nil {
			ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
			defer cancel()
			_, _ = c.Request(ctx, hc2.TypeExecStatus, hc2.U64Payload(cmd.ExecRef))
		}
	}
	cmd, _ = a.session.Command(token)
	return mcpapp.CommandStatus(cmd), nil
}

func (a *app) toolCommandRead(token, cursor string, maxBytes, maxEvents int) (any, error) {
	items, next, atEnd, err := a.session.Read(token, cursor, maxBytes, maxEvents)
	if err != nil {
		if err.Error() == "CURSOR_EXPIRED" {
			return map[string]any{"error": "CURSOR_EXPIRED", "earliest_cursor": next}, nil
		}
		return nil, err
	}
	cmd, _ := a.session.Command(token)
	return map[string]any{"items": items, "next_cursor": next, "at_end": atEnd, "status": mcpapp.CommandStatus(cmd)}, nil
}

func (a *app) toolCommandWriteStdin(token, text, b64 string) (any, error) {
	cmd, ok := a.session.Command(token)
	if !ok {
		return nil, errors.New("NOT_FOUND")
	}
	var data []byte
	if b64 != "" {
		var err error
		data, err = base64.StdEncoding.DecodeString(b64)
		if err != nil {
			return nil, err
		}
	} else {
		data = []byte(text)
	}
	p, err := hc2.StdinPayload(cmd.ExecRef, data)
	if err != nil {
		return nil, err
	}
	c, _, err := a.client()
	if err != nil {
		return nil, err
	}
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	if _, err := c.Request(ctx, hc2.TypeExecStdinData, p); err != nil {
		return nil, err
	}
	a.session.Journal("COMMAND_STDIN_WRITE", map[string]any{"cmd": token, "bytes": len(data)})
	return map[string]any{"accepted": true, "bytes": len(data)}, nil
}

func (a *app) toolCommandCloseStdin(token string) (any, error) {
	cmd, ok := a.session.Command(token)
	if !ok {
		return nil, errors.New("NOT_FOUND")
	}
	c, _, err := a.client()
	if err != nil {
		return nil, err
	}
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	if _, err := c.Request(ctx, hc2.TypeExecStdinClose, hc2.U64Payload(cmd.ExecRef)); err != nil {
		return nil, err
	}
	a.session.Journal("COMMAND_STDIN_CLOSE", map[string]any{"cmd": token})
	return map[string]any{"closed": true}, nil
}

func (a *app) toolCommandSignal(token, sig string) (any, error) {
	cmd, ok := a.session.Command(token)
	if !ok {
		return nil, errors.New("NOT_FOUND")
	}
	var s byte
	switch strings.ToUpper(sig) {
	case "INT":
		s = hc2.SigINT
	case "TERM":
		s = hc2.SigTERM
	case "KILL":
		s = hc2.SigKILL
	case "HUP":
		s = hc2.SigHUP
	default:
		return nil, errors.New("invalid signal")
	}
	c, _, err := a.client()
	if err != nil {
		return nil, err
	}
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	if _, err := c.Request(ctx, hc2.TypeExecSignal, hc2.SignalPayload(cmd.ExecRef, s)); err != nil {
		return nil, err
	}
	a.session.Journal("COMMAND_SIGNAL", map[string]any{"cmd": token, "signal": strings.ToUpper(sig)})
	return map[string]any{"sent": true}, nil
}

func (a *app) toolCommandRelease(token string) (any, error) {
	cmd, ok := a.session.Command(token)
	if !ok {
		return map[string]any{"released": true}, nil
	}
	if cmd.ExecRef != 0 && cmd.State != "LOST" {
		if c, _, err := a.client(); err == nil {
			ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
			_, _ = c.Request(ctx, hc2.TypeExecRelease, hc2.U64Payload(cmd.ExecRef))
			cancel()
		}
	}
	a.mu.Lock()
	delete(a.execRefs, cmd.ExecRef)
	a.mu.Unlock()
	a.session.Release(token)
	return map[string]any{"released": true}, nil
}

func (a *app) toolFile(typ uint16, a0, a1 string) (any, error) {
	c, caps, err := a.client()
	if err != nil {
		return nil, err
	}
	if caps&hc2.CapFile == 0 {
		return nil, errors.New("FILE capability unavailable")
	}
	p, err := hc2.EncodeTwoStrings(a0, a1)
	if err != nil {
		return nil, err
	}
	event := "FILE_PUT_START"
	if typ == hc2.TypeFileGetStart {
		event = "FILE_GET_START"
	}
	a.session.Journal(event, map[string]any{"path0": a0, "path1": a1})
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Minute)
	defer cancel()
	fr, err := c.Request(ctx, typ, p)
	if err != nil {
		return nil, err
	}
	started, ok := hc2.ParseFileStarted(fr.Payload)
	if !ok || fr.Type != hc2.TypeFileStarted {
		return nil, errors.New("bad FILE_STARTED")
	}
	ch := make(chan fileResult, 1)
	a.mu.Lock()
	a.fileWait[started.OpID] = ch
	if typ == hc2.TypeFileGetStart {
		a.fileKind[started.OpID] = "FILE_GET_DONE"
	} else {
		a.fileKind[started.OpID] = "FILE_PUT_DONE"
	}
	a.mu.Unlock()
	for {
		select {
		case res := <-ch:
			if res.err.OpID != 0 || res.err.Code != 0 {
				return nil, fmt.Errorf("FILE_ERROR %d: %s", res.err.Code, res.err.Message)
			}
			return map[string]any{"file_op_id": res.done.OpID, "bytes": res.done.Bytes, "sha256": hexString(res.done.SHA256)}, nil
		case <-ctx.Done():
			a.mu.Lock()
			delete(a.fileWait, started.OpID)
			delete(a.fileKind, started.OpID)
			a.mu.Unlock()
			return nil, ctx.Err()
		}
	}
}

type toolSpec struct {
	Name        string
	Description string
	InputSchema map[string]any
}

func tools() []toolSpec {
	return []toolSpec{
		tool("lmux_status", "Return structured lmuxd status.", map[string]any{"type": "object", "properties": map[string]any{}}),
		tool("file_put", "Upload a local host file to the target using LMUX FILE.", schemaProps("local_source", "target_destination")),
		tool("file_get", "Download a target file to a local host path using LMUX FILE.", schemaProps("target_source", "local_destination")),
		tool("command_start", "Start a private exec command with explicit argv.", map[string]any{"type": "object", "properties": map[string]any{"argv": map[string]any{"type": "array", "items": map[string]any{"type": "string"}}, "cwd": map[string]any{"type": "string"}, "kind": map[string]any{"type": "string", "description": "oneshot (default) or session"}}, "required": []string{"argv"}}),
		tool("command_status", "Return current command status.", schemaProps("command_token")),
		tool("command_read", "Read command transcript from an opaque cursor.", map[string]any{"type": "object", "properties": map[string]any{"command_token": map[string]any{"type": "string"}, "cursor": map[string]any{"type": "string"}, "max_bytes": map[string]any{"type": "integer"}, "max_events": map[string]any{"type": "integer"}}, "required": []string{"command_token"}}),
		tool("command_write_stdin", "Write bytes to command stdin using text or base64.", map[string]any{"type": "object", "properties": map[string]any{"command_token": map[string]any{"type": "string"}, "text": map[string]any{"type": "string"}, "base64": map[string]any{"type": "string"}}, "required": []string{"command_token"}}),
		tool("command_close_stdin", "Close command stdin after accepted bytes drain.", schemaProps("command_token")),
		tool("command_signal", "Send INT, TERM, KILL, or HUP to the command process group.", map[string]any{"type": "object", "properties": map[string]any{"command_token": map[string]any{"type": "string"}, "signal": map[string]any{"type": "string"}}, "required": []string{"command_token", "signal"}}),
		tool("command_release", "Release command target metadata and host transcript.", schemaProps("command_token")),
		tool("debug_capabilities", "Return GDB debug configuration, limits, and LMUX self-debug restriction.", map[string]any{"type": "object", "properties": map[string]any{}}),
		tool("debug_start", "Start a host GDB/MI debug session using target gdbserver stdio over structured EXEC.", map[string]any{"type": "object", "properties": map[string]any{"mode": enumSchema("attach", "launch"), "symbols": map[string]any{"type": "string"}, "pid": map[string]any{"type": "integer"}, "program": map[string]any{"type": "string"}, "args": map[string]any{"type": "array", "items": map[string]any{"type": "string"}}, "label": map[string]any{"type": "string"}, "remote_debug": enumSchema("off", "gdbserver", "gdb", "both")}, "required": []string{"mode", "symbols"}}),
		tool("debug_list", "List active debug sessions.", map[string]any{"type": "object", "properties": map[string]any{}}),
		tool("debug_status", "Return debug session status.", schemaProps("debug_token")),
		tool("debug_diagnostics_read", "Read paginated debug diagnostics from RSP trace, GDB/MI stream, GDB stderr, or target gdbserver stderr.", map[string]any{"type": "object", "properties": map[string]any{"debug_token": map[string]any{"type": "string"}, "source": enumSchema("rsp", "gdb_mi", "gdb_stderr", "gdbserver_stderr"), "cursor": map[string]any{"type": "string"}, "max_bytes": map[string]any{"type": "integer"}, "max_events": map[string]any{"type": "integer"}, "contains": map[string]any{"type": "string"}}, "required": []string{"debug_token", "source"}}),
		tool("debug_breakpoint_set", "Set a GDB breakpoint through MI.", map[string]any{"type": "object", "properties": map[string]any{"debug_token": map[string]any{"type": "string"}, "location": map[string]any{"type": "string"}, "temporary": map[string]any{"type": "boolean"}, "condition": map[string]any{"type": "string"}}, "required": []string{"debug_token", "location"}}),
		tool("debug_breakpoint_delete", "Delete a GDB breakpoint through MI.", schemaProps("debug_token", "breakpoint_id")),
		tool("debug_continue", "Continue the inferior through GDB/MI.", schemaProps("debug_token")),
		tool("debug_interrupt", "Interrupt the inferior through GDB/MI.", schemaProps("debug_token")),
		tool("debug_step", "Step the inferior through GDB/MI.", map[string]any{"type": "object", "properties": map[string]any{"debug_token": map[string]any{"type": "string"}, "kind": enumSchema("step", "next", "finish", "stepi", "nexti")}, "required": []string{"debug_token", "kind"}}),
		tool("debug_wait", "Wait for a debug state transition with a bounded timeout.", map[string]any{"type": "object", "properties": map[string]any{"debug_token": map[string]any{"type": "string"}, "timeout_ms": map[string]any{"type": "integer"}}, "required": []string{"debug_token"}}),
		tool("debug_threads", "List GDB threads.", schemaProps("debug_token")),
		tool("debug_select_thread", "Select a GDB thread.", schemaProps("debug_token", "thread_id")),
		tool("debug_backtrace", "Return a structured GDB backtrace.", map[string]any{"type": "object", "properties": map[string]any{"debug_token": map[string]any{"type": "string"}, "max_frames": map[string]any{"type": "integer"}}, "required": []string{"debug_token"}}),
		tool("debug_evaluate", "Evaluate a GDB expression. Expressions have normal GDB semantics and may have side effects.", schemaProps("debug_token", "expression")),
		tool("debug_registers", "Read registers through GDB/MI.", schemaProps("debug_token")),
		tool("debug_memory_read", "Read target memory through GDB/MI, bounded to 64 KiB.", map[string]any{"type": "object", "properties": map[string]any{"debug_token": map[string]any{"type": "string"}, "address": map[string]any{"type": "string"}, "length": map[string]any{"type": "integer"}}, "required": []string{"debug_token", "address", "length"}}),
		tool("debug_detach", "Detach inferior cleanly and tear down the debug session.", schemaProps("debug_token")),
		tool("debug_stop", "Emergency cleanup of host GDB, bridge, and target gdbserver execution.", schemaProps("debug_token")),
	}
}

func tool(name, desc string, schema map[string]any) toolSpec {
	return toolSpec{Name: name, Description: desc, InputSchema: schema}
}

func schemaProps(names ...string) map[string]any {
	props := map[string]any{}
	for _, n := range names {
		props[n] = map[string]any{"type": "string"}
	}
	return map[string]any{"type": "object", "properties": props, "required": names}
}

func enumSchema(vals ...string) map[string]any {
	items := make([]any, len(vals))
	for i, v := range vals {
		items[i] = v
	}
	return map[string]any{"type": "string", "enum": items}
}

func hexString(b [32]byte) string { return hex.EncodeToString(b[:]) }
