package main

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"io"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"syscall"
	"testing"
	"time"

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
	status := callTool(t, mcpSession, "lmux_status", map[string]any{})
	if !strings.Contains(mustJSON(status), "__exec") {
		t.Fatalf("status missing __exec: %v", status)
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
	get := callTool(t, mcpSession, "file_get", map[string]any{"target_source": target, "local_destination": localDst})
	if get["sha256"] != hex.EncodeToString(wantSum[:]) {
		t.Fatalf("bad get sha: %v want %s", get, hex.EncodeToString(wantSum[:]))
	}
	got, err := os.ReadFile(localDst)
	if err != nil || !bytes.Equal(got, data) {
		t.Fatalf("bad get content err=%v", err)
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
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	res, err := session.CallTool(ctx, &mcp.CallToolParams{Name: name, Arguments: args})
	if err != nil {
		t.Fatal(err)
	}
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
