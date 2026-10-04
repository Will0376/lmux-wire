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
	"strings"
	"sync"
	"time"

	"lmux/internal/hc2"
	"lmux/internal/mcpapp"

	"github.com/modelcontextprotocol/go-sdk/mcp"
)

type app struct {
	controlPath string
	manager     *mcpapp.Manager
	session     *mcpapp.Session

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
	flag.StringVar(&controlPath, "control", envDefault("LMUX_MCP_CONTROL", "/run/lmux/control-v2.sock"), "lmuxd structured control socket")
	flag.StringVar(&runtimeDir, "runtime-dir", envDefault("LMUX_MCP_RUNTIME_DIR", ""), "lmux-mcp runtime directory (default: $XDG_RUNTIME_DIR/lmux/mcp)")
	flag.StringVar(&runtimeDir, "runtime", envDefault("LMUX_MCP_RUNTIME_DIR", ""), "alias for --runtime-dir")
	flag.Parse()

	a := &app{controlPath: controlPath, manager: mcpapp.NewManager(runtimeDir), execRefs: make(map[uint64]string), fileWait: make(map[uint64]chan fileResult), fileKind: make(map[uint64]string)}
	s, err := a.manager.Start()
	if err != nil {
		fmt.Fprintf(os.Stderr, "lmux-mcp: %v\n", err)
		os.Exit(1)
	}
	a.session = s
	defer a.manager.Close()
	if err := a.ensureControl(context.Background()); err != nil {
		s.Journal("ERROR", map[string]any{"message": err.Error()})
	}
	server := newMCPServer(a)
	if err := server.Run(context.Background(), &mcp.StdioTransport{}); err != nil && !errors.Is(err, io.EOF) {
		fmt.Fprintf(os.Stderr, "lmux-mcp: %v\n", err)
	}
	s.Journal("SESSION_DISCONNECT", nil)
}

func envDefault(k, d string) string {
	if v := os.Getenv(k); v != "" {
		return v
	}
	return d
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
	token := a.execRefs[ref]
	a.mu.Unlock()
	if token != "" {
		_ = a.session.AppendOutput(token, stream, data)
	}
}

func (a *app) ExecState(st hc2.ExecState) {
	a.mu.Lock()
	token := a.execRefs[st.Ref]
	a.mu.Unlock()
	if token != "" {
		_ = a.session.UpdateState(token, st)
	}
}

func (a *app) ExecLost(ref uint64, reason uint32) {
	a.session.MarkLostByRef(ref, reason)
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
	default:
		return nil, fmt.Errorf("unknown tool %s", name)
	}
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
	if activeTotal >= mcpapp.DefaultMaxConcurrentCommand {
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

func hexString(b [32]byte) string { return hex.EncodeToString(b[:]) }
