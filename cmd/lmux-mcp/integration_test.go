package main

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
	"testing"
	"time"

	debugmgr "lmux/internal/debug"

	"github.com/modelcontextprotocol/go-sdk/mcp"
)

func TestMCPProcessIntegration(t *testing.T) {
	if os.Getenv("LMUX_MCP_IT") == "" {
		t.Skip("set LMUX_MCP_IT=1 to run process integration")
	}
	root := repoRoot(t)
	dir := t.TempDir()
	carrier := filepath.Join(dir, "carrier.sock")
	control := filepath.Join(dir, "control.sock")
	structured := filepath.Join(dir, "control-v2.sock")
	console := filepath.Join(dir, "console.sock")
	fileSock := filepath.Join(dir, "file.sock")
	runtime := filepath.Join(dir, "mcp")

	sup := startCmd(t, root, "build/host/lmuxd", "--supervisor", "--carrier-listen", carrier)
	defer stopCmd(sup)
	workerPID := waitChildPID(t, sup.Process.Pid, 10*time.Second)
	workerStopped := false
	defer func() {
		if workerStopped {
			_ = syscall.Kill(workerPID, syscall.SIGCONT)
		}
	}()
	host := startCmd(t, root, "build/host/lmuxd", "--role", "host",
		"--carrier-connect", carrier,
		"--control", control,
		"--structured-control", structured,
		"--pty", "console1:"+console+":/bin/sh",
		"--file", "files:"+fileSock)
	defer stopCmd(host)
	waitLegacyReady(t, control)

	mcpSession := startMCP(t, root, structured, runtime)
	defer mcpSession.Close()
	tools, err := mcpSession.ListTools(context.Background(), nil)
	if err != nil {
		t.Fatal(err)
	}
	if !hasTool(tools, "lmux_status") {
		t.Fatalf("tools/list missing lmux_status: %+v", tools.Tools)
	}
	if !hasTool(tools, "command_list") {
		t.Fatalf("tools/list missing command_list: %+v", tools.Tools)
	}
	if !hasTool(tools, "debug_capabilities") {
		t.Fatalf("tools/list missing debug_capabilities: %+v", tools.Tools)
	}
	for _, name := range []string{"file_put_start", "file_get_start", "file_operation_status", "file_operation_wait", "file_operation_cancel"} {
		if !hasTool(tools, name) {
			t.Fatalf("tools/list missing %s: %+v", name, tools.Tools)
		}
	}
	status := callTool(t, mcpSession, "lmux_status", map[string]any{})
	if !strings.Contains(mustJSON(status), "__exec") {
		t.Fatalf("status missing __exec: %v", status)
	}
	observability, ok := status["observability"].(map[string]any)
	if !ok || observability["supported"] != true {
		t.Fatalf("status detail unavailable: %v", status)
	}
	connection, ok := observability["connection"].(map[string]any)
	if !ok || connection["overall"] == "" || connection["lmux_state"] != "ESTABLISHED" ||
		connection["outer_local_id"] == nil || connection["lmux_local_id"] == nil {
		t.Fatalf("bad connection observability: %v", observability)
	}
	carrierStatus, ok := observability["carrier"].(map[string]any)
	if !ok || carrierStatus["name"] == "" || carrierStatus["mode"] != "raw" ||
		carrierStatus["generation"] == nil {
		t.Fatalf("bad carrier observability: %v", observability)
	}
	endpoints, ok := observability["endpoints"].([]any)
	if !ok || len(endpoints) != 1 ||
		endpoints[0].(map[string]any)["target_administrative_confirmation"] != "unknown" {
		t.Fatalf("bad endpoint observability: %v", observability)
	}
	caps := callTool(t, mcpSession, "debug_capabilities", map[string]any{})
	if caps["target_exec_capacity"] == nil || caps["self_debug_lmux_supported"] != false {
		t.Fatalf("bad debug capabilities: %v", caps)
	}
	if caps["gdb_remote_timeout_seconds"] != float64(debugmgr.DefaultGDBRemoteTimeoutSeconds) {
		t.Fatalf("debug capabilities missing remote timeout: %v", caps)
	}
	if caps["mi_async"] != true {
		t.Fatalf("debug capabilities missing mi_async: %v", caps)
	}
	if caps["interrupt_transport"] != "gdb-rsp" || caps["interrupt_stop_requires_wait"] != true {
		t.Fatalf("debug capabilities missing interrupt semantics: %v", caps)
	}
	start := callTool(t, mcpSession, "command_start", map[string]any{"argv": []string{"/bin/sh", "-c", "printf OUT; printf ERR >&2"}})
	token, _ := start["command_token"].(string)
	if token == "" {
		t.Fatalf("missing command token: %v", start)
	}
	var stdout, stderr string
	cursor, _ := start["initial_cursor"].(string)
	deadline := time.Now().Add(10 * time.Second)
	for time.Now().Before(deadline) && (stdout != "OUT" || stderr != "ERR") {
		read := callTool(t, mcpSession, "command_read", map[string]any{"command_token": token, "cursor": cursor, "max_bytes": 4096, "max_events": 64})
		if next, _ := read["next_cursor"].(string); next != "" {
			cursor = next
		}
		if rawItems, ok := read["items"].([]any); ok {
			for _, it := range rawItems {
				ev := it.(map[string]any)
				stream, _ := ev["stream"].(string)
				text, _ := ev["text"].(string)
				if stream == "stdout" {
					stdout += text
				} else if stream == "stderr" {
					stderr += text
				}
			}
		}
		time.Sleep(20 * time.Millisecond)
	}
	if stdout != "OUT" || stderr != "ERR" {
		t.Fatalf("bad transcript stdout=%q stderr=%q", stdout, stderr)
	}
	deadline = time.Now().Add(5 * time.Second)
	finalState := ""
	for time.Now().Before(deadline) {
		commandStatus := callTool(t, mcpSession, "command_status", map[string]any{"command_token": token})
		finalState, _ = commandStatus["state"].(string)
		if commandStatus["state"] == "DONE" || commandStatus["state"] == "START_FAILED" {
			break
		}
		time.Sleep(20 * time.Millisecond)
	}
	if finalState != "DONE" && finalState != "START_FAILED" {
		t.Fatalf("command did not reach a retained terminal state: %s", finalState)
	}
	listed := callTool(t, mcpSession, "command_list", map[string]any{"include_terminal": true})
	if !strings.Contains(mustJSON(listed), token) || listed["scope"] != "mcp_session" {
		t.Fatalf("command_list missing retained token %q: %v", token, listed)
	}
	callTool(t, mcpSession, "command_release", map[string]any{"command_token": token})
	listed = callTool(t, mcpSession, "command_list", map[string]any{"include_terminal": true})
	if strings.Contains(mustJSON(listed), token) {
		t.Fatalf("released command remained listed: %v", listed)
	}

	localSrc := filepath.Join(dir, "put.bin")
	localDst := filepath.Join(dir, "get.bin")
	data := bytes.Repeat([]byte("lmux-mcp-file\n"), 4096)
	if err := os.WriteFile(localSrc, data, 0600); err != nil {
		t.Fatal(err)
	}
	target := filepath.Join(os.TempDir(), "lmux-mcp-it-"+filepath.Base(dir))
	put := callTool(t, mcpSession, "file_put", map[string]any{"local_source": localSrc, "target_destination": target})
	wantSum := sha256.Sum256(data)
	if put["sha256"] != hex.EncodeToString(wantSum[:]) {
		t.Fatalf("bad put sha: %v want %s", put, hex.EncodeToString(wantSum[:]))
	}
	if opID, ok := put["operation_id"].(string); !ok || opID == "" || put["file_op_id"] != opID {
		t.Fatalf("PUT operation IDs are not identical decimal strings: %v", put)
	}
	get := callTool(t, mcpSession, "file_get", map[string]any{"target_source": target, "local_destination": localDst})
	if get["sha256"] != hex.EncodeToString(wantSum[:]) {
		t.Fatalf("bad get sha: %v want %s", get, hex.EncodeToString(wantSum[:]))
	}
	if opID, ok := get["operation_id"].(string); !ok || opID == "" || get["file_op_id"] != opID {
		t.Fatalf("GET operation IDs are not identical decimal strings: %v", get)
	}
	got, err := os.ReadFile(localDst)
	if err != nil || !bytes.Equal(got, data) {
		t.Fatalf("bad get content err=%v", err)
	}

	largeSrc := filepath.Join(dir, "large-put.bin")
	if err := os.WriteFile(largeSrc, nil, 0600); err != nil {
		t.Fatal(err)
	}
	if err := os.Truncate(largeSrc, 256*1024*1024); err != nil {
		t.Fatal(err)
	}
	largeTarget := target + "-large"
	if err := syscall.Kill(workerPID, syscall.SIGSTOP); err != nil {
		t.Fatal(err)
	}
	workerStopped = true
	const largeRequestToken = "mcp-it-large-put"
	started := callTool(t, mcpSession, "file_put_start", map[string]any{"local_source": largeSrc, "target_destination": largeTarget, "request_token": largeRequestToken})
	opID, _ := started["operation_id"].(string)
	if opID == "" || started["state"] != "RUNNING" {
		t.Fatalf("bad async FILE start: %v", started)
	}
	current := callTool(t, mcpSession, "file_operation_status", map[string]any{"request_token": largeRequestToken})
	if current["operation_id"] != opID {
		t.Fatalf("FILE token recovery mismatch: got %v want %s", current, opID)
	}
	duplicate := callTool(t, mcpSession, "file_put_start", map[string]any{"local_source": largeSrc, "target_destination": largeTarget, "request_token": largeRequestToken})
	if duplicate["operation_id"] != opID || duplicate["state"] != "RUNNING" {
		t.Fatalf("idempotent FILE duplicate mismatch: %v", duplicate)
	}
	conflict := callToolRaw(t, mcpSession, 5*time.Second, "file_put_start", map[string]any{"local_source": largeSrc, "target_destination": largeTarget + "-conflict", "request_token": largeRequestToken})
	if !conflict.IsError || !strings.Contains(toolText(conflict), "conflicts") {
		t.Fatalf("conflicting FILE token was not rejected: %s", toolText(conflict))
	}
	waitStarted := time.Now()
	waited := callToolWithTimeout(t, mcpSession, 2500*time.Millisecond, "file_operation_wait", map[string]any{"operation_id": opID, "timeout_ms": 1000})
	if waited["state"] != "RUNNING" || waited["wait_timed_out"] != true {
		t.Fatalf("deadline-aware FILE wait did not return RUNNING: %v", waited)
	}
	if time.Since(waitStarted) >= 2500*time.Millisecond {
		t.Fatalf("FILE wait did not preserve MCP response reserve")
	}
	fileStatus := callTool(t, mcpSession, "file_operation_status", map[string]any{"operation_id": opID})
	if fileStatus["state"] != "RUNNING" && fileStatus["state"] != "DONE" {
		t.Fatalf("bad async FILE status: %v", fileStatus)
	}
	if fileStatus["state"] == "RUNNING" {
		cancelled := callTool(t, mcpSession, "file_operation_cancel", map[string]any{"operation_id": opID})
		if cancelled["state"] != "CANCELLED" {
			t.Fatalf("FILE cancel not terminal: %v", cancelled)
		}
	}
	if err := syscall.Kill(workerPID, syscall.SIGCONT); err != nil {
		t.Fatal(err)
	}
	workerStopped = false

	terminalToken := "mcp-it-terminal-wait"
	terminalStarted := callTool(t, mcpSession, "file_put_start", map[string]any{"local_source": localSrc, "target_destination": target + "-terminal-wait", "request_token": terminalToken})
	terminalOp, _ := terminalStarted["operation_id"].(string)
	terminal := callTool(t, mcpSession, "file_operation_wait", map[string]any{"operation_id": terminalOp, "timeout_ms": 20000})
	if terminal["state"] != "DONE" || terminal["wait_timed_out"] == true {
		t.Fatalf("FILE wait missed terminal completion: %v", terminal)
	}
	for i := 0; i < 8; i++ {
		token := fmt.Sprintf("mcp-it-evict-%d", i)
		evict := callTool(t, mcpSession, "file_put_start", map[string]any{
			"local_source": localSrc, "target_destination": fmt.Sprintf("%s-evict-%d", target, i), "request_token": token,
		})
		evictOp, _ := evict["operation_id"].(string)
		evictDone := callTool(t, mcpSession, "file_operation_wait", map[string]any{"operation_id": evictOp, "timeout_ms": 20000})
		if evictDone["state"] != "DONE" {
			t.Fatalf("FILE eviction setup failed: %v", evictDone)
		}
	}
	unknown := callTool(t, mcpSession, "file_operation_status", map[string]any{"request_token": terminalToken})
	if unknown["state"] != "UNKNOWN" || unknown["outcome_ambiguous"] != true {
		t.Fatalf("evicted FILE token was not reported ambiguous: %v", unknown)
	}

	postCancelTarget := target + "-post-cancel"
	postCancel := callTool(t, mcpSession, "file_put", map[string]any{"local_source": localSrc, "target_destination": postCancelTarget})
	if postCancel["sha256"] != hex.EncodeToString(wantSum[:]) {
		t.Fatalf("bad post-cancel put: %v", postCancel)
	}
	if _, err := os.Stat(filepath.Join(runtime, "current", "session.log")); err != nil {
		t.Fatalf("journal not discoverable: %v", err)
	}
}

func startMCP(t *testing.T, root, control, runtime string) *mcp.ClientSession {
	t.Helper()
	cmd := exec.Command(filepath.Join(root, "build/host/lmux-mcp"), "--control", control, "--runtime-dir", runtime)
	cmd.Dir = root
	cmd.Stderr = os.Stderr
	client := mcp.NewClient(&mcp.Implementation{Name: "lmux-mcp-it", Version: "0.1"}, nil)
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	t.Cleanup(cancel)
	session, err := client.Connect(ctx, &mcp.CommandTransport{Command: cmd}, &mcp.ClientSessionOptions{ProtocolVersion: "2026-07-28"})
	if err != nil {
		t.Fatal(err)
	}
	if got := session.InitializeResult().ProtocolVersion; got != "2026-07-28" {
		t.Fatalf("protocol version = %q, want 2026-07-28", got)
	}
	return session
}

func callTool(t *testing.T, session *mcp.ClientSession, name string, args any) map[string]any {
	t.Helper()
	return callToolWithTimeout(t, session, 30*time.Second, name, args)
}

func callToolWithTimeout(t *testing.T, session *mcp.ClientSession, timeout time.Duration, name string, args any) map[string]any {
	t.Helper()
	res := callToolRaw(t, session, timeout, name, args)
	if res.IsError {
		t.Fatalf("tool %s error: %s", name, toolText(res))
	}
	var out map[string]any
	b, _ := json.Marshal(res.StructuredContent)
	if err := json.Unmarshal(b, &out); err != nil {
		t.Fatalf("bad structured content for %s: %#v: %v", name, res.StructuredContent, err)
	}
	return out
}

func callToolRaw(t *testing.T, session *mcp.ClientSession, timeout time.Duration, name string, args any) *mcp.CallToolResult {
	t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), timeout)
	defer cancel()
	res, err := session.CallTool(ctx, &mcp.CallToolParams{Name: name, Arguments: args})
	if err != nil {
		t.Fatal(err)
	}
	return res
}

func waitChildPID(t *testing.T, parent int, timeout time.Duration) int {
	t.Helper()
	path := filepath.Join("/proc", strconv.Itoa(parent), "task", strconv.Itoa(parent), "children")
	deadline := time.Now().Add(timeout)
	for time.Now().Before(deadline) {
		data, err := os.ReadFile(path)
		if err == nil {
			for _, field := range strings.Fields(string(data)) {
				pid, parseErr := strconv.Atoi(field)
				if parseErr == nil && pid > 0 {
					return pid
				}
			}
		}
		time.Sleep(20 * time.Millisecond)
	}
	t.Fatalf("supervisor %d did not create a worker", parent)
	return 0
}

func hasTool(list *mcp.ListToolsResult, name string) bool {
	for _, tool := range list.Tools {
		if tool.Name == name {
			return true
		}
	}
	return false
}

func toolText(res *mcp.CallToolResult) string {
	if len(res.Content) == 0 {
		return ""
	}
	if txt, ok := res.Content[0].(*mcp.TextContent); ok {
		return txt.Text
	}
	return mustJSON(res.Content)
}

func repoRoot(t *testing.T) string {
	t.Helper()
	wd, err := os.Getwd()
	if err != nil {
		t.Fatal(err)
	}
	for {
		if _, err := os.Stat(filepath.Join(wd, "Makefile")); err == nil {
			return wd
		}
		parent := filepath.Dir(wd)
		if parent == wd {
			t.Fatal("repo root not found")
		}
		wd = parent
	}
}

func startCmd(t *testing.T, root string, name string, args ...string) *exec.Cmd {
	t.Helper()
	cmd := exec.Command(filepath.Join(root, name), args...)
	cmd.Dir = root
	cmd.Stdout = os.Stderr
	cmd.Stderr = os.Stderr
	cmd.SysProcAttr = &syscall.SysProcAttr{Setpgid: true}
	if err := cmd.Start(); err != nil {
		t.Fatal(err)
	}
	return cmd
}

func stopCmd(cmd *exec.Cmd) {
	if cmd == nil || cmd.Process == nil {
		return
	}
	_ = syscall.Kill(-cmd.Process.Pid, syscall.SIGKILL)
	_ = cmd.Wait()
}

func waitLegacyReady(t *testing.T, path string) {
	t.Helper()
	deadline := time.Now().Add(15 * time.Second)
	for time.Now().Before(deadline) {
		txt, err := legacyStatus(path)
		if err == nil && strings.Contains(txt, "__exec") && strings.Contains(txt, "files") && strings.Contains(txt, "LMUX/2") {
			return
		}
		time.Sleep(100 * time.Millisecond)
	}
	txt, _ := legacyStatus(path)
	t.Fatalf("legacy ready timeout status=%s", txt)
}

func legacyStatus(path string) (string, error) {
	ctx, cancel := context.WithTimeout(context.Background(), time.Second)
	defer cancel()
	var d net.Dialer
	c, err := d.DialContext(ctx, "unix", path)
	if err != nil {
		return "", err
	}
	defer c.Close()
	_ = c.SetDeadline(time.Now().Add(time.Second))
	if _, err := c.Write([]byte("STATUS\n")); err != nil {
		return "", err
	}
	b, err := io.ReadAll(c)
	if err != nil && len(b) == 0 {
		return string(b), err
	}
	return string(b), nil
}
