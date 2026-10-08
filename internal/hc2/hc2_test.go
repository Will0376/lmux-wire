package hc2

import (
	"bufio"
	"bytes"
	"encoding/binary"
	"io"
	"net"
	"testing"
)

func buildFrame(t *testing.T, typ uint16, req uint64, payload []byte) []byte {
	t.Helper()
	var hdr [HeaderLen]byte
	binary.LittleEndian.PutUint32(hdr[0:4], Magic)
	binary.LittleEndian.PutUint16(hdr[4:6], Version)
	binary.LittleEndian.PutUint16(hdr[6:8], typ)
	binary.LittleEndian.PutUint64(hdr[12:20], req)
	binary.LittleEndian.PutUint32(hdr[20:24], uint32(len(payload)))
	out := append([]byte{}, hdr[:]...)
	out = append(out, payload...)
	return out
}

func TestReadFrameFragmentedAndCoalesced(t *testing.T) {
	a, b := net.Pipe()
	defer a.Close()
	defer b.Close()
	done := make(chan error, 1)
	go func() {
		f1 := buildFrame(t, TypeExecStdout, 7, []byte("a\x00b"))
		f2 := buildFrame(t, TypeStatusReply, 8, []byte("ok"))
		for _, chunk := range [][]byte{f1[:5], f1[5:17], f1[17:], f2} {
			if _, err := a.Write(chunk); err != nil {
				done <- err
				return
			}
		}
		done <- nil
	}()
	r := bufio.NewReader(b)
	fr, err := readFrame(r)
	if err != nil {
		t.Fatal(err)
	}
	if fr.Type != TypeExecStdout || fr.RequestID != 7 || !bytes.Equal(fr.Payload, []byte("a\x00b")) {
		t.Fatalf("bad first frame: %+v", fr)
	}
	fr, err = readFrame(r)
	if err != nil {
		t.Fatal(err)
	}
	if fr.Type != TypeStatusReply || fr.RequestID != 8 || string(fr.Payload) != "ok" {
		t.Fatalf("bad second frame: %+v", fr)
	}
	if err := <-done; err != nil {
		t.Fatal(err)
	}
}

func TestReadFrameEOFMidFrame(t *testing.T) {
	f := buildFrame(t, TypeStatusReply, 1, []byte("payload"))
	r := bufio.NewReader(bytes.NewReader(f[:HeaderLen+2]))
	_, err := readFrame(r)
	if err == nil || err == io.EOF {
		t.Fatalf("want unexpected eof, got %v", err)
	}
}

func TestEncodeRequests(t *testing.T) {
	p, err := EncodeExecStart("/tmp", []string{"/bin/echo", "x"})
	if err != nil {
		t.Fatal(err)
	}
	if len(p) == 0 || len(p) > MaxPayload {
		t.Fatalf("bad exec payload len %d", len(p))
	}
	p, err = EncodeTwoStrings("/host/a", "/target/b")
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Contains(p, []byte("/target/b")) {
		t.Fatalf("encoded payload missing path")
	}
	if _, err := StdinPayload(1, make([]byte, MaxPayload)); err == nil {
		t.Fatalf("oversized stdin accepted")
	}
}

func TestDispatchExecOutputCopiesPayload(t *testing.T) {
	h := &captureHandler{}
	c := &Client{handler: h}
	payload := make([]byte, 8+3)
	binary.LittleEndian.PutUint64(payload[:8], 42)
	copy(payload[8:], "AAA")
	c.dispatch(Frame{Type: TypeExecStdout, Payload: payload})
	copy(payload[8:], "BBB")
	c.dispatch(Frame{Type: TypeExecStdout, Payload: payload})
	if got := string(bytes.Join(h.outputs, nil)); got != "AAABBB" {
		t.Fatalf("dispatch retained borrowed payload: got %q want AAABBB", got)
	}
}

type captureHandler struct {
	outputs [][]byte
}

func (h *captureHandler) ExecOutput(_ string, _ uint64, data []byte) {
	h.outputs = append(h.outputs, data)
}

func (h *captureHandler) ExecState(ExecState)         {}
func (h *captureHandler) ExecLost(uint64, uint32)     {}
func (h *captureHandler) FileProgress(uint64, uint64) {}
func (h *captureHandler) FileDone(FileDone)           {}
func (h *captureHandler) FileError(FileError)         {}
