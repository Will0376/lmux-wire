package main

import (
	"bytes"
	"context"
	"encoding/json"
	"io"
	"testing"
	"time"

	"lmux/internal/hc2"
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
		"lmux_status":            false,
		"file_put":               false,
		"file_get":               false,
		"file_put_start":         false,
		"file_get_start":         false,
		"file_operation_status":  false,
		"file_operation_wait":    false,
		"file_operation_cancel":  false,
		"command_start":          false,
		"command_status":         false,
		"command_read":           false,
		"command_write_stdin":    false,
		"command_close_stdin":    false,
		"command_signal":         false,
		"command_release":        false,
		"debug_capabilities":     false,
		"debug_start":            false,
		"debug_status":           false,
		"debug_diagnostics_read": false,
		"debug_detach":           false,
		"debug_stop":             false,
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

func TestFileOperationIDJSONRoundTrip(t *testing.T) {
	tests := []struct {
		id   uint64
		text string
	}{
		{4980636128614556090, "4980636128614556090"},
		{4980636128614556091, "4980636128614556091"},
		{4980636128614556092, "4980636128614556092"},
		{^uint64(0), "18446744073709551615"},
	}
	seen := make(map[string]bool)
	for _, tt := range tests {
		v, err := parseFileOperationID(tt.text)
		if err != nil || v != tt.id {
			t.Fatalf("parseFileOperationID(%q) = %d, %v", tt.text, v, err)
		}
		op := hc2.FileOperation{OpID: tt.id, State: hc2.FileDoneState, Kind: 1}
		outputs := []struct {
			name   string
			value  map[string]any
			fields []string
		}{
			{"start-status-wait-cancel", fileOperationResult(op), []string{"operation_id"}},
			{"legacy-put-get", completedFileResult(op, "known-token"), []string{"operation_id", "file_op_id"}},
		}
		for _, output := range outputs {
			structuredWire, err := json.Marshal(output.value)
			if err != nil {
				t.Fatalf("marshal %s for %s: %v", output.name, tt.text, err)
			}
			wires := []struct {
				name string
				data []byte
			}{
				{"structuredContent", structuredWire},
				{"textContent", []byte(mustJSON(output.value))},
			}
			for _, wire := range wires {
				var decoded map[string]any
				if err := json.Unmarshal(wire.data, &decoded); err != nil {
					t.Fatalf("unmarshal %s %s for %s: %v", wire.name, output.name, tt.text, err)
				}
				for _, field := range output.fields {
					got, ok := decoded[field].(string)
					if !ok || got != tt.text {
						t.Fatalf("%s %s %s after JSON round trip = %#v, want string %q; JSON=%s", wire.name, output.name, field, decoded[field], tt.text, wire.data)
					}
				}
				if output.name == "start-status-wait-cancel" && wire.name == "structuredContent" {
					seen[decoded["operation_id"].(string)] = true
				}
			}
		}
	}
	if len(seen) != len(tests) {
		t.Fatalf("distinct uint64 operation IDs collapsed after JSON round trip: %#v", seen)
	}
	if _, err := parseFileOperationID("0"); err == nil {
		t.Fatal("zero operation ID accepted")
	}
	if _, err := parseFileOperationID("18446744073709551616"); err == nil {
		t.Fatal("overflowing operation ID accepted")
	}
}

func TestFileOperationIDInputSchemasRequireStrings(t *testing.T) {
	want := map[string]bool{
		"file_operation_status": false,
		"file_operation_wait":   false,
		"file_operation_cancel": false,
	}
	for _, spec := range tools() {
		if _, ok := want[spec.Name]; !ok {
			continue
		}
		properties, ok := spec.InputSchema["properties"].(map[string]any)
		if !ok {
			t.Fatalf("%s properties = %#v", spec.Name, spec.InputSchema["properties"])
		}
		opID, ok := properties["operation_id"].(map[string]any)
		if !ok || opID["type"] != "string" {
			t.Fatalf("%s operation_id schema = %#v, want string", spec.Name, properties["operation_id"])
		}
		want[spec.Name] = true
	}
	for name, found := range want {
		if !found {
			t.Fatalf("missing FILE operation tool schema %s", name)
		}
	}
}

func TestFileRequestTokenAndWaitDeadline(t *testing.T) {
	if err := requireFileLifecycle(hc2.CapFile); err == nil {
		t.Fatal("v1.0.0 FILE-only daemon accepted for v1.0.1 lifecycle operations")
	}
	if err := requireFileLifecycle(hc2.CapFile | hc2.CapFileLifecycle); err != nil {
		t.Fatalf("v1.0.1 FILE lifecycle capability rejected: %v", err)
	}
	if err := validateFileRequestToken(""); err == nil {
		t.Fatal("empty FILE request token accepted")
	}
	if err := validateFileRequestToken("known-before-start"); err != nil {
		t.Fatal(err)
	}
	if err := validateFileRequestToken("bad\x00token"); err == nil {
		t.Fatal("NUL FILE request token accepted")
	}
	if got, err := fileOperationWaitDuration(context.Background(), 0); err != nil || got != 15*time.Second {
		t.Fatalf("default FILE wait = %v, %v", got, err)
	}
	if _, err := fileOperationWaitDuration(context.Background(), 20001); err == nil {
		t.Fatal("oversized FILE wait accepted")
	}
	ctx, cancel := context.WithDeadline(context.Background(), time.Now().Add(4*time.Second))
	defer cancel()
	got, err := fileOperationWaitDuration(ctx, 20000)
	if err != nil || got < 2*time.Second || got > 2600*time.Millisecond {
		t.Fatalf("deadline-aware FILE wait = %v, %v", got, err)
	}
}

func TestGDBRemoteTimeoutValidation(t *testing.T) {
	if err := validateGDBRemoteTimeoutSeconds(10); err != nil {
		t.Fatalf("default timeout rejected: %v", err)
	}
	if err := validateGDBRemoteTimeoutSeconds(1); err != nil {
		t.Fatalf("minimum timeout rejected: %v", err)
	}
	if err := validateGDBRemoteTimeoutSeconds(300); err != nil {
		t.Fatalf("maximum timeout rejected: %v", err)
	}
	for _, bad := range []int{0, -1, 301} {
		if err := validateGDBRemoteTimeoutSeconds(bad); err == nil {
			t.Fatalf("timeout %d should be rejected", bad)
		}
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
