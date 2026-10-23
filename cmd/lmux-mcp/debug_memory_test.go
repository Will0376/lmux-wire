package main

import (
	"bytes"
	"encoding/base64"
	"encoding/json"
	"strings"
	"testing"

	debugmgr "lmux/internal/debug"
)

func TestDebugMemoryReadDefaultBase64Compatibility(t *testing.T) {
	data := []byte{0x00, 0x41, 0xff}
	got, err := presentDebugMemoryRead(debugmgr.MemoryReadResult{
		Address: "0x1000",
		Data:    data,
	}, "")
	if err != nil {
		t.Fatal(err)
	}
	wire, err := json.Marshal(got)
	if err != nil {
		t.Fatal(err)
	}
	var decoded map[string]any
	if err := json.Unmarshal(wire, &decoded); err != nil {
		t.Fatal(err)
	}
	if len(decoded) != 2 || decoded["base64"] != "AEH/" || decoded["bytes"] != float64(len(data)) {
		t.Fatalf("default response changed: %s", wire)
	}
	if _, ok := decoded["hex"]; ok {
		t.Fatalf("default response contains hex: %s", wire)
	}
	if _, ok := decoded["address"]; ok {
		t.Fatalf("default response contains new address field: %s", wire)
	}
}

func TestDebugMemoryHexDumpKnownBytesAndAddresses(t *testing.T) {
	data := []byte{
		0x00, 0x41, 0x1f, 0x20, 0x7e, 0x7f, 0xff, 0x30,
		0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38,
		0x42, 0x00, 0x43,
	}
	dump, err := debugMemoryHexDump(0x1ff8, data)
	if err != nil {
		t.Fatal(err)
	}
	want := "0000000000001ff8  00 41 1f 20 7e 7f ff 30  31 32 33 34 35 36 37 38  |.A. ~..012345678|\n" +
		"0000000000002008  42 00 43 " + strings.Repeat(" ", 41) + "|B.C|"
	if dump != want {
		t.Fatalf("hex dump mismatch:\n got %q\nwant %q", dump, want)
	}
}

func TestDebugMemoryHexDumpAddressBoundaries(t *testing.T) {
	const maxUint64 = ^uint64(0)
	dump, err := debugMemoryHexDump(maxUint64, []byte{0})
	if err != nil {
		t.Fatal(err)
	}
	if !strings.HasPrefix(dump, "ffffffffffffffff  00 ") {
		t.Fatalf("maximum address formatted incorrectly: %q", dump)
	}
	if _, err := debugMemoryHexDump(maxUint64-15, make([]byte, 16)); err != nil {
		t.Fatalf("valid terminal address range rejected: %v", err)
	}
	if _, err := debugMemoryHexDump(maxUint64-15, make([]byte, 17)); err == nil {
		t.Fatal("overflowing address range accepted")
	}
}

func TestDebugMemoryReadBothPreservesBytes(t *testing.T) {
	data := []byte{0x00, 0x01, 'A', 'z', 0x7f, 0xff}
	got, err := presentDebugMemoryRead(debugmgr.MemoryReadResult{
		Address: "0xfffffff0",
		Data:    data,
	}, "both")
	if err != nil {
		t.Fatal(err)
	}
	wire, err := json.Marshal(got)
	if err != nil {
		t.Fatal(err)
	}
	var decoded map[string]any
	if err := json.Unmarshal(wire, &decoded); err != nil {
		t.Fatal(err)
	}
	b64, ok := decoded["base64"].(string)
	if !ok {
		t.Fatalf("both response has no base64 string: %s", wire)
	}
	roundTrip, err := base64.StdEncoding.DecodeString(b64)
	if err != nil || !bytes.Equal(roundTrip, data) {
		t.Fatalf("base64 changed source bytes: got %x want %x err=%v", roundTrip, data, err)
	}
	if decoded["address"] != "0xfffffff0" ||
		decoded["bytes"] != float64(len(data)) ||
		!strings.Contains(decoded["hex"].(string), "00 01 41 7a 7f ff") ||
		!strings.HasSuffix(decoded["hex"].(string), "|..Az..|") {
		t.Fatalf("bad both response: %s", wire)
	}
}

func TestDebugMemoryReadFormatAndSizeLimits(t *testing.T) {
	if _, err := normalizeDebugMemoryFormat("raw"); err == nil {
		t.Fatal("invalid memory format accepted")
	}
	for _, format := range []string{"", "base64", "hex", "both"} {
		if _, err := normalizeDebugMemoryFormat(format); err != nil {
			t.Fatalf("format %q rejected: %v", format, err)
		}
	}
	data := make([]byte, debugmgr.MaxMemoryRead)
	got, err := presentDebugMemoryRead(debugmgr.MemoryReadResult{
		Address: "0x0",
		Data:    data,
	}, "both")
	if err != nil {
		t.Fatalf("maximum bounded result rejected: %v", err)
	}
	maxHexBytes := ((debugmgr.MaxMemoryRead + 15) / 16) * 87
	if len(got["hex"].(string)) > maxHexBytes {
		t.Fatalf("hex response exceeded bound: %d > %d", len(got["hex"].(string)), maxHexBytes)
	}
	if _, err := presentDebugMemoryRead(debugmgr.MemoryReadResult{
		Address: "0x0",
		Data:    make([]byte, debugmgr.MaxMemoryRead+1),
	}, "hex"); err == nil {
		t.Fatal("oversized memory result accepted")
	}
}

func TestDebugMemoryReadToolSchema(t *testing.T) {
	for _, spec := range tools() {
		if spec.Name != "debug_memory_read" {
			continue
		}
		properties := spec.InputSchema["properties"].(map[string]any)
		format := properties["format"].(map[string]any)
		length := properties["length"].(map[string]any)
		if format["type"] != "string" || len(format["enum"].([]any)) != 3 {
			t.Fatalf("bad format schema: %#v", format)
		}
		if length["minimum"] != 1 || length["maximum"] != debugmgr.MaxMemoryRead {
			t.Fatalf("bad length schema: %#v", length)
		}
		return
	}
	t.Fatal("debug_memory_read tool schema missing")
}
