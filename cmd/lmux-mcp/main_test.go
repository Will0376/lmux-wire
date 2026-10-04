package main

import (
	"bytes"
	"context"
	"encoding/json"
	"io"
	"testing"
	"time"

	"lmux/internal/mcpapp"

	"github.com/modelcontextprotocol/go-sdk/mcp"
)

func TestSDKToolDiscoveryAndCall(t *testing.T) {
	a := testApp(t)
	server := newMCPServer(a)
	clientSession := connectInMemory(t, server)
	defer clientSession.Close()

	tools, err := clientSession.ListTools(context.Background(), nil)
	if err != nil {
		t.Fatal(err)
	}
	want := map[string]bool{
		"lmux_status":         false,
		"file_put":            false,
		"file_get":            false,
		"command_start":       false,
		"command_status":      false,
		"command_read":        false,
		"command_write_stdin": false,
		"command_close_stdin": false,
		"command_signal":      false,
		"command_release":     false,
	}
	for _, tool := range tools.Tools {
		if _, exists := want[tool.Name]; exists {
			want[tool.Name] = true
		}
	}
	for name, seen := range want {
		if !seen {
			t.Fatalf("missing tool %s", name)
		}
	}

	res, err := clientSession.CallTool(context.Background(), &mcp.CallToolParams{
		Name:      "command_release",
		Arguments: map[string]any{"command_token": "missing-token"},
	})
	if err != nil {
		t.Fatal(err)
	}
	if res.IsError {
		t.Fatalf("command_release returned tool error: %+v", res)
	}
	got, ok := res.StructuredContent.(map[string]any)
	if !ok || got["released"] != true {
		t.Fatalf("bad structured content: %#v", res.StructuredContent)
	}
}

func TestSDKStdioEmptyEOFProducesNoStdout(t *testing.T) {
	a := testApp(t)
	server := newMCPServer(a)
	var stdout bytes.Buffer
	ctx, cancel := context.WithTimeout(context.Background(), time.Second)
	defer cancel()
	err := server.Run(ctx, &mcp.IOTransport{
		Reader: io.NopCloser(bytes.NewReader(nil)),
		Writer: nopWriteCloser{&stdout},
	})
	if err != nil && stdout.Len() == 0 {
		return
	}
	if stdout.Len() != 0 {
		t.Fatalf("unexpected stdout bytes on empty input: %q", stdout.String())
	}
}

func TestSDKMalformedInputDoesNotWritePlaintextStdout(t *testing.T) {
	a := testApp(t)
	server := newMCPServer(a)
	var stdout bytes.Buffer
	ctx, cancel := context.WithTimeout(context.Background(), time.Second)
	defer cancel()
	_ = server.Run(ctx, &mcp.IOTransport{
		Reader: io.NopCloser(bytes.NewBufferString("not-json\n")),
		Writer: nopWriteCloser{&stdout},
	})
	if bytes.Contains(stdout.Bytes(), []byte("lmux-mcp")) {
		t.Fatalf("stdout contains non-SDK/plaintext data: %q", stdout.String())
	}
}

func TestToolResultContentIsJSONText(t *testing.T) {
	a := testApp(t)
	server := newMCPServer(a)
	clientSession := connectInMemory(t, server)
	defer clientSession.Close()

	res, err := clientSession.CallTool(context.Background(), &mcp.CallToolParams{Name: "command_release"})
	if err != nil {
		t.Fatal(err)
	}
	if len(res.Content) == 0 {
		t.Fatal("missing text content")
	}
	text, ok := res.Content[0].(*mcp.TextContent)
	if !ok {
		t.Fatalf("content is %T, want TextContent", res.Content[0])
	}
	var decoded map[string]any
	if err := json.Unmarshal([]byte(text.Text), &decoded); err != nil {
		t.Fatalf("content is not JSON text: %q: %v", text.Text, err)
	}
}

func testApp(t *testing.T) *app {
	t.Helper()
	m := mcpapp.NewManager(t.TempDir())
	s, err := m.Start()
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(m.Close)
	return &app{
		controlPath: "/tmp/nonexistent-control-v2.sock",
		manager:     m,
		session:     s,
		execRefs:    make(map[uint64]string),
		fileWait:    make(map[uint64]chan fileResult),
		fileKind:    make(map[uint64]string),
	}
}

func connectInMemory(t *testing.T, server *mcp.Server) *mcp.ClientSession {
	t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	t.Cleanup(cancel)
	t1, t2 := mcp.NewInMemoryTransports()
	serverSession, err := server.Connect(ctx, t1, nil)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = serverSession.Close() })
	client := mcp.NewClient(&mcp.Implementation{Name: "lmux-mcp-test", Version: "0.1"}, nil)
	clientSession, err := client.Connect(ctx, t2, &mcp.ClientSessionOptions{ProtocolVersion: "2026-07-28"})
	if err != nil {
		t.Fatal(err)
	}
	if got := clientSession.InitializeResult().ProtocolVersion; got != "2026-07-28" {
		t.Fatalf("protocol version = %q, want 2026-07-28", got)
	}
	return clientSession
}

type nopWriteCloser struct {
	io.Writer
}

func (n nopWriteCloser) Close() error { return nil }
