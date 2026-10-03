package main

import (
	"bufio"
	"bytes"
	"encoding/json"
	"testing"
)

func TestRPCFramingRoundTrip(t *testing.T) {
	var b bytes.Buffer
	msg := rpcMessage{JSONRPC: "2.0", ID: json.RawMessage(`1`), Method: "tools/list"}
	if err := writeRPC(bufio.NewWriter(&b), msg); err != nil {
		t.Fatal(err)
	}
	got, err := readRPC(bufio.NewReader(&b))
	if err != nil {
		t.Fatal(err)
	}
	if got.Method != "tools/list" || string(got.ID) != "1" {
		t.Fatalf("bad rpc roundtrip: %+v", got)
	}
}

func TestToolsIncludeRequiredSurface(t *testing.T) {
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
	for _, tool := range tools() {
		if name, ok := tool["name"].(string); ok {
			if _, exists := want[name]; exists {
				want[name] = true
			}
		}
	}
	for name, seen := range want {
		if !seen {
			t.Fatalf("missing tool %s", name)
		}
	}
}
