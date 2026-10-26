package main

import (
	"context"
	"crypto/rand"
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

var buildVersion = "development"

type app struct {
	controlPath string
	manager     *mcpapp.Manager
	session     *mcpapp.Session
	debug       *debugmgr.Manager
	debugConfig debugmgr.Config

	mu            sync.Mutex
	hc            *hc2.Client
	caps          uint32
	execRefs      map[uint64]string
	fileJournaled map[uint64]byte
	fileStarted   map[uint64]bool
	fileNotify    chan struct{}
}

func main() {
	var controlPath, runtimeDir string
	var gdbPath, targetGDBServer, gdbSysroot, gdbSolib string
	var maxDebugSessions, gdbRemoteTimeout int
	var showVersion bool
	flag.StringVar(&controlPath, "control", envDefault("LMUX_MCP_CONTROL", "/run/lmux/control-v2.sock"), "lmuxd structured control socket")
	flag.StringVar(&runtimeDir, "runtime-dir", envDefault("LMUX_MCP_RUNTIME_DIR", ""), "lmux-mcp runtime directory (default: $XDG_RUNTIME_DIR/lmux/mcp)")
	flag.StringVar(&runtimeDir, "runtime", envDefault("LMUX_MCP_RUNTIME_DIR", ""), "alias for --runtime-dir")
	flag.StringVar(&gdbPath, "gdb", envDefault("LMUX_MCP_GDB", ""), "host GDB executable for debug tools")
	flag.StringVar(&targetGDBServer, "target-gdbserver", envDefault("LMUX_MCP_TARGET_GDBSERVER", ""), "absolute target gdbserver path for debug tools")
	flag.IntVar(&maxDebugSessions, "max-debug-sessions", 2, "maximum active debug sessions")
	flag.StringVar(&gdbSysroot, "gdb-sysroot", envDefault("LMUX_MCP_GDB_SYSROOT", ""), "optional GDB sysroot")
	flag.StringVar(&gdbSolib, "gdb-solib-search-path", envDefault("LMUX_MCP_GDB_SOLIB_SEARCH_PATH", ""), "optional GDB solib-search-path")
	flag.IntVar(&gdbRemoteTimeout, "gdb-remote-timeout", envIntDefault("LMUX_MCP_GDB_REMOTE_TIMEOUT", debugmgr.DefaultGDBRemoteTimeoutSeconds), "GDB remotetimeout in seconds for target remote negotiation (1..300)")
	flag.BoolVar(&showVersion, "version", false, "print version and exit")
	flag.Parse()
	if showVersion {
		fmt.Printf("lmux-mcp %s (LMUX Wire)\n", buildVersion)
		return
	}
	if err := validateGDBRemoteTimeoutSeconds(gdbRemoteTimeout); err != nil {
		fmt.Fprintf(os.Stderr, "lmux-mcp: %v\n", err)
		os.Exit(2)
	}

	a := &app{controlPath: controlPath, manager: mcpapp.NewManager(runtimeDir), execRefs: make(map[uint64]string), fileJournaled: make(map[uint64]byte), fileStarted: make(map[uint64]bool), fileNotify: make(chan struct{}, 1)}
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

func normalizeDebugMemoryFormat(format string) (string, error) {
	if format == "" {
		return "base64", nil
	}
	switch format {
	case "base64", "hex", "both":
		return format, nil
	default:
		return "", errors.New("INVALID_ARGUMENT: format must be base64, hex, or both")
	}
}

func debugMemoryHexDump(address uint64, data []byte) (string, error) {
	const bytesPerLine = 16
	if len(data) == 0 {
		return "", nil
	}
	lines := (len(data) + bytesPerLine - 1) / bytesPerLine
	var out strings.Builder
	// A full line is 86 display bytes plus one separator newline.
	out.Grow(lines * 87)
	for offset := 0; offset < len(data); offset += bytesPerLine {
		if uint64(offset) > ^uint64(0)-address {
			return "", errors.New("INVALID_ARGUMENT: memory range exceeds uint64 address space")
		}
		if offset != 0 {
			out.WriteByte('\n')
		}
		fmt.Fprintf(&out, "%016x  ", address+uint64(offset))
		for i := 0; i < bytesPerLine; i++ {
			if i == 8 {
				out.WriteByte(' ')
			}
			if offset+i < len(data) {
				fmt.Fprintf(&out, "%02x ", data[offset+i])
			} else {
				out.WriteString("   ")
			}
		}
		out.WriteString(" |")
		for i := 0; i < bytesPerLine && offset+i < len(data); i++ {
			c := data[offset+i]
			if c >= 0x20 && c <= 0x7e {
				out.WriteByte(c)
			} else {
				out.WriteByte('.')
			}
		}
		out.WriteByte('|')
	}
	return out.String(), nil
}

func presentDebugMemoryRead(result debugmgr.MemoryReadResult, format string) (map[string]any, error) {
	format, err := normalizeDebugMemoryFormat(format)
	if err != nil {
		return nil, err
	}
	if len(result.Data) > debugmgr.MaxMemoryRead {
		return nil, errors.New("INVALID_ARGUMENT: memory result exceeds size limit")
	}
	out := map[string]any{"bytes": len(result.Data)}
	if format == "base64" || format == "both" {
		out["base64"] = base64.StdEncoding.EncodeToString(result.Data)
	}
	if format == "hex" || format == "both" {
		address, err := strconv.ParseUint(result.Address, 0, 64)
		if err != nil {
			return nil, fmt.Errorf("invalid memory address returned by GDB: %q", result.Address)
		}
		dump, err := debugMemoryHexDump(address, result.Data)
		if err != nil {
			return nil, err
		}
		out["address"] = fmt.Sprintf("0x%x", address)
		out["hex"] = dump
	}
	return out, nil
}

func (a *app) ensureControl(ctx context.Context) error {
	a.mu.Lock()
	if a.hc != nil {
		closed, _ := a.hc.Closed()
		if !closed {
			a.mu.Unlock()
			return nil
		}
		a.hc = nil
		a.caps = 0
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
	} else {
		a.debug.SetHC(c)
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
	a.session.Journal("FILE_OPERATION_PROGRESS", map[string]any{"operation_id": formatFileOperationID(opID), "bytes": bytes})
}

func (a *app) FileDone(done hc2.FileDone) {
	a.journalFileOperation(hc2.FileOperation{OpID: done.OpID, State: hc2.FileDoneState, Bytes: done.Bytes, SHA256: done.SHA256})
	a.session.Journal("FILE_DONE", map[string]any{"file_op_id": formatFileOperationID(done.OpID), "bytes": done.Bytes, "sha256": hexString(done.SHA256)})
	a.signalFileStateChange()
}

func (a *app) FileError(err hc2.FileError) {
	state := hc2.FileFailed
	if err.Code == hc2.ErrTargetLost || err.Code == hc2.ErrStaleTarget {
		state = hc2.FileLost
	}
	a.journalFileOperation(hc2.FileOperation{OpID: err.OpID, State: state, ErrorCode: err.Code, Error: err.Message})
	a.session.Journal("FILE_ERROR", map[string]any{"file_op_id": formatFileOperationID(err.OpID), "code": err.Code, "message": err.Message})
	a.signalFileStateChange()
}

func (a *app) signalFileStateChange() {
	a.mu.Lock()
	if a.fileNotify == nil {
		a.fileNotify = make(chan struct{}, 1)
	}
	ch := a.fileNotify
	a.mu.Unlock()
	select {
	case ch <- struct{}{}:
	default:
	}
}

func (a *app) fileStateChanges() <-chan struct{} {
	a.mu.Lock()
	defer a.mu.Unlock()
	if a.fileNotify == nil {
		a.fileNotify = make(chan struct{}, 1)
	}
	return a.fileNotify
}

func (a *app) journalFileOperation(op hc2.FileOperation) {
	if op.OpID == 0 || op.State == hc2.FileRunning {
		return
	}
	a.mu.Lock()
	if a.fileJournaled == nil {
		a.fileJournaled = make(map[uint64]byte)
	}
	if a.fileJournaled[op.OpID] == op.State {
		a.mu.Unlock()
		return
	}
	if len(a.fileJournaled) >= 64 {
		for id := range a.fileJournaled {
			delete(a.fileJournaled, id)
			break
		}
	}
	a.fileJournaled[op.OpID] = op.State
	a.mu.Unlock()
	event := "FILE_OPERATION_FAILED"
	switch op.State {
	case hc2.FileDoneState:
		event = "FILE_OPERATION_DONE"
	case hc2.FileCancelled:
		event = "FILE_OPERATION_CANCELLED"
	case hc2.FileLost:
		event = "FILE_OPERATION_LOST"
	}
	fields := map[string]any{
		"operation_id": formatFileOperationID(op.OpID),
		"bytes":        op.Bytes,
	}
	if op.ErrorCode != 0 || op.Error != "" {
		fields["code"] = op.ErrorCode
		fields["message"] = op.Error
	}
	if op.State == hc2.FileDoneState {
		fields["sha256"] = hexString(op.SHA256)
	}
	a.session.Journal(event, fields)
}

func (a *app) journalFileStart(op hc2.FileOperation) {
	if op.OpID == 0 {
		return
	}
	a.mu.Lock()
	if a.fileStarted == nil {
		a.fileStarted = make(map[uint64]bool)
	}
	if a.fileStarted[op.OpID] {
		a.mu.Unlock()
		return
	}
	if len(a.fileStarted) >= 64 {
		for id := range a.fileStarted {
			delete(a.fileStarted, id)
			break
		}
	}
	a.fileStarted[op.OpID] = true
	a.mu.Unlock()
	fields := map[string]any{
		"operation_id":      formatFileOperationID(op.OpID),
		"direction":         fileKindName(op.Kind),
		"local_path":        op.LocalPath,
		"target_path":       op.TargetPath,
		"total_bytes_known": op.TotalKnown,
		"incarnation":       op.Incarnation,
	}
	if op.TotalKnown {
		fields["total_bytes"] = op.Total
	}
	a.session.Journal("FILE_OPERATION_START", fields)
}

func mustJSON(v any) string {
	b, err := json.MarshalIndent(v, "", "  ")
	if err != nil {
		return fmt.Sprintf("%v", v)
	}
	return string(b)
}

func newMCPServer(a *app) *mcp.Server {
	server := mcp.NewServer(&mcp.Implementation{Name: "lmux-mcp", Version: buildVersion}, nil)
	for _, spec := range tools() {
		name := spec.Name
		server.AddTool(&mcp.Tool{
			Name:        spec.Name,
			Description: spec.Description,
			InputSchema: spec.InputSchema,
		}, func(ctx context.Context, req *mcp.CallToolRequest) (*mcp.CallToolResult, error) {
			out, err := a.callTool(ctx, name, req.Params.Arguments)
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

func (a *app) callTool(ctx context.Context, name string, args json.RawMessage) (any, error) {
	switch name {
	case "lmux_status":
		return a.toolStatus()
	case "command_list":
		var p struct {
			IncludeTerminal bool `json:"include_terminal"`
		}
		if err := json.Unmarshal(args, &p); err != nil {
			return nil, err
		}
		return a.toolCommandList(p.IncludeTerminal)
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
			RequestToken      string `json:"request_token"`
		}
		if err := json.Unmarshal(args, &p); err != nil {
			return nil, err
		}
		return a.toolFile(ctx, hc2.TypeFilePutStart, p.LocalSource, p.TargetDestination, p.RequestToken)
	case "file_get":
		var p struct {
			TargetSource     string `json:"target_source"`
			LocalDestination string `json:"local_destination"`
			RequestToken     string `json:"request_token"`
		}
		if err := json.Unmarshal(args, &p); err != nil {
			return nil, err
		}
		return a.toolFile(ctx, hc2.TypeFileGetStart, p.TargetSource, p.LocalDestination, p.RequestToken)
	case "file_put_start":
		var p struct {
			LocalSource       string `json:"local_source"`
			TargetDestination string `json:"target_destination"`
			RequestToken      string `json:"request_token"`
		}
		if err := json.Unmarshal(args, &p); err != nil {
			return nil, err
		}
		return a.toolFileStart(ctx, hc2.TypeFilePutStart, p.LocalSource, p.TargetDestination, p.RequestToken)
	case "file_get_start":
		var p struct {
			TargetSource     string `json:"target_source"`
			LocalDestination string `json:"local_destination"`
			RequestToken     string `json:"request_token"`
		}
		if err := json.Unmarshal(args, &p); err != nil {
			return nil, err
		}
		return a.toolFileStart(ctx, hc2.TypeFileGetStart, p.TargetSource, p.LocalDestination, p.RequestToken)
	case "file_operation_status":
		var p struct {
			OperationID  string `json:"operation_id"`
			RequestToken string `json:"request_token"`
		}
		if err := json.Unmarshal(args, &p); err != nil {
			return nil, err
		}
		if p.OperationID != "" && p.RequestToken != "" {
			return nil, errors.New("provide either operation_id or request_token, not both")
		}
		if p.RequestToken != "" {
			return a.toolFileOperationStatusToken(ctx, p.RequestToken)
		}
		var opID uint64
		if p.OperationID != "" {
			var err error
			opID, err = parseFileOperationID(p.OperationID)
			if err != nil {
				return nil, err
			}
		}
		return a.toolFileOperationStatus(ctx, opID)
	case "file_operation_wait":
		var p struct {
			OperationID string `json:"operation_id"`
			TimeoutMS   int    `json:"timeout_ms"`
		}
		if err := json.Unmarshal(args, &p); err != nil {
			return nil, err
		}
		opID, err := parseFileOperationID(p.OperationID)
		if err != nil {
			return nil, err
		}
		return a.toolFileOperationWait(ctx, opID, p.TimeoutMS)
	case "file_operation_cancel":
		var p struct {
			OperationID string `json:"operation_id"`
		}
		if err := json.Unmarshal(args, &p); err != nil {
			return nil, err
		}
		opID, err := parseFileOperationID(p.OperationID)
		if err != nil {
			return nil, err
		}
		return a.toolFileOperationCancel(ctx, opID)
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
			Format     string `json:"format"`
		}
		if err := json.Unmarshal(args, &p); err != nil {
			return nil, err
		}
		format, err := normalizeDebugMemoryFormat(p.Format)
		if err != nil {
			return nil, err
		}
		return a.withDebug(p.DebugToken, func(s *debugmgr.Session) (any, error) {
			ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
			defer cancel()
			result, err := s.MemoryRead(ctx, p.Address, p.Length)
			if err != nil {
				return nil, err
			}
			return presentDebugMemoryRead(result, format)
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
	st, c, caps, err := a.requestLegacyStatus()
	if err != nil {
		return nil, err
	}
	if caps&hc2.CapStatusDetail == 0 {
		st.Observability = hc2.UnsupportedObservability(
			"connected lmuxd does not advertise HC2 STATUS_DETAIL")
		return st, nil
	}
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	fr, err := c.Request(ctx, hc2.TypeStatusDetailRequest, nil)
	if err != nil {
		return nil, err
	}
	if fr.Type != hc2.TypeStatusDetailReply {
		return nil, fmt.Errorf("unexpected STATUS_DETAIL response frame %d", fr.Type)
	}
	st.Observability, err = hc2.DecodeStatusDetail(fr.Payload)
	if err != nil {
		return nil, fmt.Errorf("decode STATUS_DETAIL: %w", err)
	}
	return st, nil
}

func (a *app) requestLegacyStatus() (hc2.Status, *hc2.Client, uint32, error) {
	c, caps, err := a.client()
	if err != nil {
		return hc2.Status{}, nil, 0, err
	}
	if caps&hc2.CapStatus == 0 {
		return hc2.Status{}, nil, caps, errors.New("STATUS capability unavailable")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	fr, err := c.Request(ctx, hc2.TypeStatusRequest, nil)
	if err != nil {
		return hc2.Status{}, nil, caps, err
	}
	if fr.Type != hc2.TypeStatusReply {
		return hc2.Status{}, nil, caps,
			fmt.Errorf("unexpected STATUS response frame %d", fr.Type)
	}
	st, err := hc2.DecodeStatus(fr.Payload)
	return st, c, caps, err
}

const (
	commandListLimit = 128
	commandFreshness = 5 * time.Second
)

func commandTerminalState(state string) bool {
	switch state {
	case "DONE", "FAILED", "START_FAILED", "LOST":
		return true
	default:
		return false
	}
}

func (a *app) commandTargetAvailability() string {
	st, _, _, err := a.requestLegacyStatus()
	if err != nil {
		return "unknown"
	}
	// Standby endpoint health is intentionally irrelevant.
	if st.Outer.State == hc2.OuterStateEstablished &&
		st.LMUX.State == hc2.LMUXStateEstablished {
		return "online"
	}
	return "offline"
}

func (a *app) toolCommandList(includeTerminal bool) (any, error) {
	commands, total := a.session.ListCommands(includeTerminal, commandListLimit)
	availability := "unknown"
	if len(commands) != 0 {
		availability = a.commandTargetAvailability()
	}
	return commandListResult(a.session.ID, commands, total, includeTerminal,
		availability, time.Now()), nil
}

func commandListResult(sessionID string, commands []mcpapp.CommandSnapshot,
	total int, includeTerminal bool, availability string, now time.Time) map[string]any {
	items := make([]map[string]any, 0, len(commands))
	for _, cmd := range commands {
		terminal := commandTerminalState(cmd.State)
		verified := !cmd.LastVerified.IsZero() &&
			(terminal || (availability == "online" &&
				now.Sub(cmd.LastVerified) <= commandFreshness))
		verification := "unverified"
		if terminal && verified {
			verification = "terminal"
		} else if verified {
			verification = "current"
		} else if !cmd.LastVerified.IsZero() {
			verification = "stale"
		}
		item := map[string]any{
			"token":                      cmd.Token,
			"state":                      cmd.State,
			"kind":                       cmd.Kind,
			"execution_id":               strconv.FormatUint(cmd.TargetID, 10),
			"exec_ref":                   strconv.FormatUint(cmd.ExecRef, 10),
			"reattachable":               !terminal && cmd.ExecRef != 0,
			"terminal":                   terminal,
			"state_verified":             verified,
			"verification":               verification,
			"target_availability":        availability,
			"process_identity_available": false,
		}
		if !cmd.LastVerified.IsZero() {
			item["last_verified_at"] =
				cmd.LastVerified.UTC().Format(time.RFC3339Nano)
		}
		if cmd.LostReason != "" {
			item["lost_reason"] = cmd.LostReason
		}
		items = append(items, item)
	}
	return map[string]any{
		"commands": items, "include_terminal": includeTerminal,
		"scope": "mcp_session", "session_id": sessionID,
		"target_availability": availability,
		"bounded_limit":       commandListLimit, "total_matching": total,
		"verification_freshness_ms": commandFreshness.Milliseconds(),
		"truncated":                 total > len(items),
	}
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
	return s.Status(), nil
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

func validateFileRequestToken(token string) error {
	if token == "" {
		return errors.New("request_token is required and must be chosen before FILE START")
	}
	if len(token) >= 128 || strings.IndexByte(token, 0) >= 0 {
		return errors.New("request_token must be 1..127 bytes and contain no NUL")
	}
	return nil
}

func requireFileLifecycle(caps uint32) error {
	if caps&hc2.CapFileLifecycle == 0 {
		return errors.New("connected lmuxd lacks HC2 FILE lifecycle/idempotency capability required by LMUX Wire 1.0.1")
	}
	return nil
}

func newInternalFileRequestToken() (string, error) {
	var b [16]byte
	if _, err := rand.Read(b[:]); err != nil {
		return "", err
	}
	return "legacy-" + hex.EncodeToString(b[:]), nil
}

func (a *app) startFileOperation(ctx context.Context, typ uint16, a0, a1, requestToken string) (hc2.FileOperation, error) {
	if err := validateFileRequestToken(requestToken); err != nil {
		return hc2.FileOperation{}, err
	}
	c, caps, err := a.client()
	if err != nil {
		return hc2.FileOperation{}, err
	}
	if err := requireFileLifecycle(caps); err != nil {
		return hc2.FileOperation{}, err
	}
	p, err := hc2.EncodeFileStart(a.session.ID, requestToken, a0, a1)
	if err != nil {
		return hc2.FileOperation{}, err
	}
	requestType := hc2.TypeFilePutStartIdempotent
	event := "FILE_PUT_START"
	if typ == hc2.TypeFileGetStart {
		requestType = hc2.TypeFileGetStartIdempotent
		event = "FILE_GET_START"
	}
	a.session.Journal(event, map[string]any{"path0": a0, "path1": a1, "request_token": requestToken})
	requestCtx, cancel := context.WithTimeout(ctx, 5*time.Second)
	defer cancel()
	fr, err := c.Request(requestCtx, requestType, p)
	if err != nil {
		return hc2.FileOperation{}, fmt.Errorf("FILE START outcome may be ambiguous; recover with request_token %q and do not retry with a new token: %w", requestToken, err)
	}
	op, ok := hc2.ParseFileOperation(fr.Payload)
	if !ok || fr.Type != hc2.TypeFileState {
		return hc2.FileOperation{}, errors.New("bad FILE_STATE for idempotent START")
	}
	a.journalFileStart(op)
	a.journalFileOperation(op)
	return op, nil
}

func (a *app) toolFileStart(ctx context.Context, typ uint16, a0, a1, requestToken string) (any, error) {
	op, err := a.startFileOperation(ctx, typ, a0, a1, requestToken)
	if err != nil {
		return nil, err
	}
	out := fileOperationResult(op)
	out["request_token"] = requestToken
	out["idempotent"] = true
	return out, nil
}

func fileStateName(state byte) string {
	switch state {
	case hc2.FileRunning:
		return "RUNNING"
	case hc2.FileDoneState:
		return "DONE"
	case hc2.FileFailed:
		return "FAILED"
	case hc2.FileCancelled:
		return "CANCELLED"
	case hc2.FileLost:
		return "LOST"
	default:
		return "UNKNOWN"
	}
}

func fileKindName(kind byte) string {
	if kind == 2 {
		return "GET"
	}
	return "PUT"
}

func parseFileOperationID(s string) (uint64, error) {
	if s == "" {
		return 0, errors.New("operation_id is required")
	}
	v, err := strconv.ParseUint(s, 10, 64)
	if err != nil || v == 0 {
		return 0, errors.New("operation_id must be a non-zero decimal string")
	}
	return v, nil
}

func formatFileOperationID(id uint64) string {
	return strconv.FormatUint(id, 10)
}

func fileOperationResult(op hc2.FileOperation) map[string]any {
	out := map[string]any{
		"operation_id": formatFileOperationID(op.OpID), "direction": fileKindName(op.Kind),
		"state": fileStateName(op.State), "bytes_transferred": op.Bytes,
		"total_bytes_known": op.TotalKnown, "local_path": op.LocalPath,
		"target_path": op.TargetPath, "incarnation": op.Incarnation,
		"lmux_local_conn": op.LocalConn, "lmux_peer_conn": op.PeerConn,
	}
	if op.TotalKnown {
		out["total_bytes"] = op.Total
	}
	if op.ErrorCode != 0 || op.Error != "" {
		out["error_code"] = op.ErrorCode
		out["error"] = op.Error
	}
	if op.State == hc2.FileDoneState {
		out["sha256"] = hexString(op.SHA256)
	}
	return out
}

func completedFileResult(op hc2.FileOperation, requestToken string) map[string]any {
	id := formatFileOperationID(op.OpID)
	return map[string]any{
		// file_op_id is the pre-lifecycle compatibility name. It must remain a
		// string: JSON numbers cannot represent every HC2 uint64 operation ID.
		"file_op_id":    id,
		"operation_id":  id,
		"request_token": requestToken,
		"bytes":         op.Bytes,
		"sha256":        hexString(op.SHA256),
	}
}

func (a *app) requestFileOperation(ctx context.Context, typ uint16, opID uint64, retryReadOnly bool) (hc2.FileOperation, error) {
	if opID == 0 && typ != hc2.TypeFileStatus {
		return hc2.FileOperation{}, errors.New("operation_id must be non-zero")
	}
	attempts := 1
	if retryReadOnly {
		attempts = 2
	}
	var lastErr error
	for attempt := 0; attempt < attempts; attempt++ {
		c, caps, err := a.client()
		if err != nil {
			lastErr = err
			continue
		}
		if err := requireFileLifecycle(caps); err != nil {
			return hc2.FileOperation{}, err
		}
		requestCtx, cancel := context.WithTimeout(ctx, 5*time.Second)
		fr, err := c.Request(requestCtx, typ, hc2.U64Payload(opID))
		cancel()
		if err != nil {
			lastErr = err
			continue
		}
		op, ok := hc2.ParseFileOperation(fr.Payload)
		if !ok || fr.Type != hc2.TypeFileState {
			return hc2.FileOperation{}, errors.New("bad FILE_STATE")
		}
		return op, nil
	}
	return hc2.FileOperation{}, lastErr
}

func (a *app) requestFileOperationToken(ctx context.Context, requestToken string) (hc2.FileOperation, error) {
	if err := validateFileRequestToken(requestToken); err != nil {
		return hc2.FileOperation{}, err
	}
	p, err := hc2.EncodeFileToken(a.session.ID, requestToken)
	if err != nil {
		return hc2.FileOperation{}, err
	}
	var lastErr error
	for attempt := 0; attempt < 2; attempt++ {
		c, caps, err := a.client()
		if err != nil {
			lastErr = err
			continue
		}
		if err := requireFileLifecycle(caps); err != nil {
			return hc2.FileOperation{}, err
		}
		requestCtx, cancel := context.WithTimeout(ctx, 5*time.Second)
		fr, err := c.Request(requestCtx, hc2.TypeFileStatusToken, p)
		cancel()
		if err != nil {
			var hcErr *hc2.Error
			if errors.As(err, &hcErr) {
				return hc2.FileOperation{}, err
			}
			lastErr = err
			continue
		}
		op, ok := hc2.ParseFileOperation(fr.Payload)
		if !ok || fr.Type != hc2.TypeFileState {
			return hc2.FileOperation{}, errors.New("bad FILE_STATE")
		}
		return op, nil
	}
	return hc2.FileOperation{}, lastErr
}

func (a *app) toolFileOperationStatus(ctx context.Context, opID uint64) (any, error) {
	op, err := a.requestFileOperation(ctx, hc2.TypeFileStatus, opID, true)
	if err != nil {
		return nil, err
	}
	a.journalFileStart(op)
	a.journalFileOperation(op)
	return fileOperationResult(op), nil
}

func (a *app) toolFileOperationStatusToken(ctx context.Context, requestToken string) (any, error) {
	op, err := a.requestFileOperationToken(ctx, requestToken)
	if err != nil {
		var hcErr *hc2.Error
		if errors.As(err, &hcErr) && hcErr.Code == hc2.ErrNotFound {
			return map[string]any{
				"request_token":     requestToken,
				"state":             "UNKNOWN",
				"outcome_ambiguous": true,
				"warning":           "request token is unknown or expired; this does not prove FILE START was not executed; do not retry automatically",
			}, nil
		}
		return nil, err
	}
	a.journalFileStart(op)
	a.journalFileOperation(op)
	out := fileOperationResult(op)
	out["request_token"] = requestToken
	out["outcome_ambiguous"] = false
	return out, nil
}

func fileOperationWaitDuration(ctx context.Context, timeoutMS int) (time.Duration, error) {
	const (
		defaultWait     = 15 * time.Second
		maxWait         = 20 * time.Second
		responseReserve = 1500 * time.Millisecond
	)
	if timeoutMS == 0 {
		timeoutMS = int(defaultWait / time.Millisecond)
	}
	if timeoutMS < 0 || time.Duration(timeoutMS)*time.Millisecond > maxWait {
		return 0, errors.New("timeout_ms must be in range 0..20000")
	}
	waitFor := time.Duration(timeoutMS) * time.Millisecond
	if deadline, ok := ctx.Deadline(); ok {
		safe := time.Until(deadline) - responseReserve
		if safe < 0 {
			safe = 0
		}
		if safe < waitFor {
			waitFor = safe
		}
	}
	return waitFor, nil
}

func (a *app) toolFileOperationWait(ctx context.Context, opID uint64, timeoutMS int) (any, error) {
	_, err := fileOperationWaitDuration(ctx, timeoutMS)
	if err != nil {
		return nil, err
	}
	op, err := a.requestFileOperation(ctx, hc2.TypeFileStatus, opID, true)
	if err != nil {
		return nil, err
	}
	a.journalFileStart(op)
	if op.State != hc2.FileRunning {
		a.journalFileOperation(op)
		return fileOperationResult(op), nil
	}
	waitFor, _ := fileOperationWaitDuration(ctx, timeoutMS)
	if waitFor == 0 {
		out := fileOperationResult(op)
		out["wait_timed_out"] = true
		return out, nil
	}
	timer := time.NewTimer(waitFor)
	defer timer.Stop()
	for {
		select {
		case <-ctx.Done():
			return nil, ctx.Err()
		case <-a.fileStateChanges():
			op, err = a.requestFileOperation(ctx, hc2.TypeFileStatus, opID, true)
			if err != nil {
				return nil, err
			}
			if op.State != hc2.FileRunning {
				a.journalFileOperation(op)
				return fileOperationResult(op), nil
			}
		case <-timer.C:
			last, statusErr := a.requestFileOperation(ctx, hc2.TypeFileStatus, opID, true)
			if statusErr != nil {
				return nil, statusErr
			}
			a.journalFileStart(last)
			if last.State != hc2.FileRunning {
				a.journalFileOperation(last)
				return fileOperationResult(last), nil
			}
			out := fileOperationResult(last)
			out["wait_timed_out"] = true
			return out, nil
		}
	}
}

func (a *app) toolFileOperationCancel(ctx context.Context, opID uint64) (any, error) {
	op, err := a.requestFileOperation(ctx, hc2.TypeFileCancel, opID, false)
	if err != nil {
		return nil, fmt.Errorf("FILE cancellation result unknown; query operation status: %w", err)
	}
	a.journalFileStart(op)
	a.journalFileOperation(op)
	a.signalFileStateChange()
	return fileOperationResult(op), nil
}

func (a *app) toolFile(ctx context.Context, typ uint16, a0, a1, requestToken string) (any, error) {
	if requestToken == "" {
		var err error
		requestToken, err = newInternalFileRequestToken()
		if err != nil {
			return nil, err
		}
	}
	started, err := a.startFileOperation(ctx, typ, a0, a1, requestToken)
	if err != nil {
		return nil, err
	}
	waitCtx, cancel := context.WithTimeout(ctx, 5*time.Minute)
	defer cancel()
	for {
		op, err := a.requestFileOperation(waitCtx, hc2.TypeFileStatus, started.OpID, true)
		if err != nil {
			return nil, fmt.Errorf("FILE operation %d (request_token %q) may still be running; query status before retrying: %w", started.OpID, requestToken, err)
		}
		a.journalFileStart(op)
		a.journalFileOperation(op)
		switch op.State {
		case hc2.FileDoneState:
			return completedFileResult(op, requestToken), nil
		case hc2.FileFailed, hc2.FileCancelled, hc2.FileLost:
			return nil, fmt.Errorf("FILE operation %d %s (code %d): %s", op.OpID, fileStateName(op.State), op.ErrorCode, op.Error)
		}
		// Terminal notifications are the normal wake-up path. The slow
		// reconciliation timer covers completion while HC2 was disconnected.
		timer := time.NewTimer(5 * time.Second)
		select {
		case <-waitCtx.Done():
			timer.Stop()
			return nil, fmt.Errorf("FILE operation %d (request_token %q) remains RUNNING after request deadline; use file_operation_status or file_operation_wait", started.OpID, requestToken)
		case <-a.fileStateChanges():
			timer.Stop()
		case <-timer.C:
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
		tool("file_put", "Upload a local host file to the target using LMUX FILE. Optional request_token enables caller-known recovery.", fileToolSchema("local_source", "target_destination", false)),
		tool("file_get", "Download a target file to a local host path using LMUX FILE. Optional request_token enables caller-known recovery.", fileToolSchema("target_source", "local_destination", false)),
		tool("file_put_start", "Idempotently start a daemon-owned FILE upload using a caller-supplied request token.", fileToolSchema("local_source", "target_destination", true)),
		tool("file_get_start", "Idempotently start a daemon-owned FILE download using a caller-supplied request token.", fileToolSchema("target_source", "local_destination", true)),
		tool("file_operation_status", "Return retained FILE state by operation_id or caller request_token; omit both only to diagnose the active operation.", map[string]any{"type": "object", "properties": map[string]any{"operation_id": map[string]any{"type": "string", "description": "Opaque decimal operation ID"}, "request_token": map[string]any{"type": "string", "minLength": 1, "maxLength": 127, "description": "Caller-supplied token from FILE START"}}}),
		tool("file_operation_wait", "Wait up to 20 seconds (15 seconds by default and less than the MCP context deadline) for a FILE operation to become terminal.", map[string]any{"type": "object", "properties": map[string]any{"operation_id": map[string]any{"type": "string", "description": "Opaque decimal operation ID"}, "timeout_ms": map[string]any{"type": "integer", "minimum": 0, "maximum": 20000}}, "required": []string{"operation_id"}}),
		tool("file_operation_cancel", "Request cancellation of a FILE operation and return its authoritative resulting state.", map[string]any{"type": "object", "properties": map[string]any{"operation_id": map[string]any{"type": "string", "description": "Opaque decimal operation ID"}}, "required": []string{"operation_id"}}),
		tool("command_start", "Start a private exec command with explicit argv.", map[string]any{"type": "object", "properties": map[string]any{"argv": map[string]any{"type": "array", "items": map[string]any{"type": "string"}}, "cwd": map[string]any{"type": "string"}, "kind": map[string]any{"type": "string", "description": "oneshot (default) or session"}}, "required": []string{"argv"}}),
		tool("command_list", "List retained commands owned by this MCP process session. Terminal commands are omitted by default.", map[string]any{"type": "object", "properties": map[string]any{"include_terminal": map[string]any{"type": "boolean"}}}),
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
		tool("debug_memory_read", "Read target memory through GDB/MI, bounded to 64 KiB, as base64, a readable hex dump, or both.", map[string]any{"type": "object", "properties": map[string]any{"debug_token": map[string]any{"type": "string"}, "address": map[string]any{"type": "string"}, "length": map[string]any{"type": "integer", "minimum": 1, "maximum": debugmgr.MaxMemoryRead}, "format": enumSchema("base64", "hex", "both")}, "required": []string{"debug_token", "address", "length"}}),
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

func fileToolSchema(path0, path1 string, requireToken bool) map[string]any {
	props := map[string]any{
		path0: map[string]any{"type": "string"},
		path1: map[string]any{"type": "string"},
		"request_token": map[string]any{
			"type": "string", "minLength": 1, "maxLength": 127,
			"description": "Caller-generated idempotency token known before START",
		},
	}
	required := []string{path0, path1}
	if requireToken {
		required = append(required, "request_token")
	}
	return map[string]any{"type": "object", "properties": props, "required": required}
}

func enumSchema(vals ...string) map[string]any {
	items := make([]any, len(vals))
	for i, v := range vals {
		items[i] = v
	}
	return map[string]any{"type": "string", "enum": items}
}

func hexString(b [32]byte) string { return hex.EncodeToString(b[:]) }
