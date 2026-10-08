package hc2

import (
	"bufio"
	"bytes"
	"context"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"net"
	"os"
	"sync"
	"sync/atomic"
	"time"
)

const (
	Magic      uint32 = 0x3243484c
	Version    uint16 = 1
	HeaderLen         = 24
	MaxPayload        = 4096

	TypeClientHello    uint16 = 1
	TypeServerHello    uint16 = 2
	TypeError          uint16 = 3
	TypeStatusRequest  uint16 = 10
	TypeStatusReply    uint16 = 11
	TypeExecStart      uint16 = 20
	TypeExecStarted    uint16 = 21
	TypeExecAttach     uint16 = 22
	TypeExecStatus     uint16 = 23
	TypeExecStdinData  uint16 = 24
	TypeExecStdinClose uint16 = 25
	TypeExecSignal     uint16 = 26
	TypeExecRelease    uint16 = 27
	TypeExecStdout     uint16 = 40
	TypeExecStderr     uint16 = 41
	TypeExecState      uint16 = 42
	TypeExecLost       uint16 = 43
	TypeExecStdinReady uint16 = 44
	TypeFilePutStart   uint16 = 60
	TypeFileGetStart   uint16 = 61
	TypeFileStarted    uint16 = 62
	TypeFileDone       uint16 = 63
	TypeFileError      uint16 = 64
	TypeFileProgress   uint16 = 65

	CapStatus uint32 = 1 << 0
	CapExec   uint32 = 1 << 1
	CapFile   uint32 = 1 << 2

	ErrOK                 uint32 = 0
	ErrInvalidArgument    uint32 = 1
	ErrNotConnected       uint32 = 2
	ErrServiceUnavailable uint32 = 3
	ErrNotFound           uint32 = 4
	ErrStaleTarget        uint32 = 5
	ErrBusy               uint32 = 6
	ErrResourceLimit      uint32 = 7
	ErrTargetLost         uint32 = 8
	ErrIO                 uint32 = 9
	ErrProtocol           uint32 = 10
	ErrInternal           uint32 = 11

	SigINT  byte = 1
	SigTERM byte = 2
	SigKILL byte = 3
	SigHUP  byte = 4
)

type Frame struct {
	Type      uint16
	Flags     uint16
	RequestID uint64
	Payload   []byte
}

type Error struct {
	Code    uint32
	Message string
}

func (e *Error) Error() string {
	if e.Message == "" {
		return fmt.Sprintf("hc2 error %d", e.Code)
	}
	return fmt.Sprintf("hc2 error %d: %s", e.Code, e.Message)
}

type ExecState struct {
	Ref          uint64
	TargetExecID uint64
	State        byte
	Fail         byte
	Errno        uint16
	ExitCode     uint32
	TermSignal   uint32
	Flags        uint32
}

type Status struct {
	BuildVersion string       `json:"build_version"`
	CarrierOpen  bool         `json:"carrier_open"`
	CarrierRX    uint64       `json:"carrier_rx_bytes"`
	CarrierTX    uint64       `json:"carrier_tx_bytes"`
	Outer        StatusOuter  `json:"outer"`
	LMUX         StatusLMUX   `json:"lmux"`
	Factories    Factories    `json:"factories"`
	Services     []ServiceRow `json:"services"`
}

type StatusOuter struct {
	State      byte   `json:"state"`
	LocalConn  uint64 `json:"local_conn"`
	PeerConn   uint64 `json:"peer_conn"`
	CRCErrors  uint64 `json:"crc_errors"`
	SeqErrors  uint64 `json:"seq_errors"`
	ResyncDrop uint64 `json:"resync_drops"`
}

type StatusLMUX struct {
	State            byte   `json:"state"`
	LocalConn        uint64 `json:"local_conn"`
	PeerConn         uint64 `json:"peer_conn"`
	EstablishedCount uint64 `json:"established_count"`
}

type Factories struct {
	PTY            bool `json:"pty"`
	UnixRendezvous bool `json:"unix_rendezvous"`
	File           bool `json:"file"`
	Exec           bool `json:"exec"`
}

type ServiceRow struct {
	Name         string `json:"name"`
	Type         string `json:"type"`
	Acknowledged bool   `json:"acknowledged"`
	ServiceID    uint64 `json:"service_id"`
	Internal     bool   `json:"internal"`
}

type FileStarted struct {
	OpID          uint64
	Kind          byte
	ExpectedBytes uint64
}

type FileDone struct {
	OpID   uint64
	Bytes  uint64
	SHA256 [32]byte
}

type FileError struct {
	OpID    uint64
	Code    uint32
	Message string
}

type Handler interface {
	ExecOutput(stream string, ref uint64, data []byte)
	ExecState(state ExecState)
	ExecLost(ref uint64, reason uint32)
	FileProgress(opID, bytes uint64)
	FileDone(done FileDone)
	FileError(err FileError)
}

type ExecStdinReadyHandler interface {
	ExecStdinReady(ref uint64)
}

type Client struct {
	conn net.Conn
	br   *bufio.Reader
	bw   *bufio.Writer

	nextReq atomic.Uint64
	handler Handler

	writeMu sync.Mutex

	mu      sync.Mutex
	pending map[uint64]chan Frame
	closed  bool
	err     error
}

func Dial(ctx context.Context, path string, h Handler) (*Client, uint32, error) {
	d := net.Dialer{}
	conn, err := d.DialContext(ctx, "unix", path)
	if err != nil {
		return nil, 0, err
	}
	c := &Client{
		conn:    conn,
		br:      bufio.NewReader(conn),
		bw:      bufio.NewWriter(conn),
		handler: h,
		pending: make(map[uint64]chan Frame),
	}
	c.nextReq.Store(1)
	if err := c.writeFrame(Frame{Type: TypeClientHello, RequestID: c.nextID()}); err != nil {
		conn.Close()
		return nil, 0, err
	}
	fr, err := readFrame(c.br)
	if err != nil {
		conn.Close()
		return nil, 0, err
	}
	if fr.Type != TypeServerHello || len(fr.Payload) < 14 {
		conn.Close()
		return nil, 0, fmt.Errorf("bad server hello")
	}
	caps := binary.LittleEndian.Uint32(fr.Payload)
	go c.readLoop()
	return c, caps, nil
}

func (c *Client) Close() error {
	c.mu.Lock()
	c.closed = true
	for id, ch := range c.pending {
		delete(c.pending, id)
		close(ch)
	}
	c.mu.Unlock()
	return c.conn.Close()
}

func (c *Client) nextID() uint64 {
	id := c.nextReq.Add(1)
	if id == 0 {
		id = c.nextReq.Add(1)
	}
	return id
}

func (c *Client) Request(ctx context.Context, typ uint16, payload []byte) (Frame, error) {
	req := c.nextID()
	ch := make(chan Frame, 4)
	c.mu.Lock()
	if c.closed {
		err := c.err
		c.mu.Unlock()
		if err == nil {
			err = io.ErrClosedPipe
		}
		return Frame{}, err
	}
	c.pending[req] = ch
	c.mu.Unlock()
	if err := c.writeFrame(Frame{Type: typ, RequestID: req, Payload: payload}); err != nil {
		c.forget(req)
		return Frame{}, err
	}
	for {
		select {
		case <-ctx.Done():
			c.forget(req)
			return Frame{}, ctx.Err()
		case fr, ok := <-ch:
			if !ok {
				return Frame{}, io.ErrClosedPipe
			}
			if fr.Type == TypeError {
				e := DecodeError(fr.Payload)
				if e.Code == ErrOK {
					return fr, nil
				}
				return fr, &e
			}
			return fr, nil
		}
	}
}

func (c *Client) forget(req uint64) {
	c.mu.Lock()
	delete(c.pending, req)
	c.mu.Unlock()
}

func (c *Client) writeFrame(fr Frame) error {
	if len(fr.Payload) > MaxPayload {
		return fmt.Errorf("payload too large")
	}
	var hdr [HeaderLen]byte
	binary.LittleEndian.PutUint32(hdr[0:4], Magic)
	binary.LittleEndian.PutUint16(hdr[4:6], Version)
	binary.LittleEndian.PutUint16(hdr[6:8], fr.Type)
	binary.LittleEndian.PutUint16(hdr[8:10], fr.Flags)
	binary.LittleEndian.PutUint64(hdr[12:20], fr.RequestID)
	binary.LittleEndian.PutUint32(hdr[20:24], uint32(len(fr.Payload)))
	c.writeMu.Lock()
	defer c.writeMu.Unlock()
	if _, err := c.bw.Write(hdr[:]); err != nil {
		return err
	}
	if len(fr.Payload) > 0 {
		if _, err := c.bw.Write(fr.Payload); err != nil {
			return err
		}
	}
	return c.bw.Flush()
}

func (c *Client) readLoop() {
	for {
		fr, err := readFrame(c.br)
		if err != nil {
			c.closeWith(err)
			return
		}
		if fr.RequestID != 0 {
			c.mu.Lock()
			ch := c.pending[fr.RequestID]
			if ch != nil {
				delete(c.pending, fr.RequestID)
			}
			c.mu.Unlock()
			if ch != nil {
				ch <- fr
				close(ch)
				continue
			}
		}
		c.dispatch(fr)
	}
}

func (c *Client) closeWith(err error) {
	c.mu.Lock()
	c.closed = true
	c.err = err
	for id, ch := range c.pending {
		delete(c.pending, id)
		close(ch)
	}
	c.mu.Unlock()
}

func (c *Client) dispatch(fr Frame) {
	if c.handler == nil {
		return
	}
	switch fr.Type {
	case TypeExecStdout, TypeExecStderr:
		if len(fr.Payload) < 8 {
			return
		}
		stream := "stdout"
		if fr.Type == TypeExecStderr {
			stream = "stderr"
		}
		ref := binary.LittleEndian.Uint64(fr.Payload[:8])
		data := append([]byte(nil), fr.Payload[8:]...)
		c.handler.ExecOutput(stream, ref, data)
	case TypeExecState:
		if st, ok := DecodeExecState(fr.Payload); ok {
			c.handler.ExecState(st)
		}
	case TypeExecLost:
		if len(fr.Payload) >= 12 {
			c.handler.ExecLost(binary.LittleEndian.Uint64(fr.Payload[:8]),
				binary.LittleEndian.Uint32(fr.Payload[8:12]))
		}
	case TypeExecStdinReady:
		if len(fr.Payload) >= 8 {
			if h, ok := c.handler.(ExecStdinReadyHandler); ok {
				h.ExecStdinReady(binary.LittleEndian.Uint64(fr.Payload[:8]))
			}
		}
	case TypeFileProgress:
		if len(fr.Payload) >= 16 {
			c.handler.FileProgress(binary.LittleEndian.Uint64(fr.Payload[:8]),
				binary.LittleEndian.Uint64(fr.Payload[8:16]))
		}
	case TypeFileDone:
		if done, ok := ParseFileDone(fr.Payload); ok {
			c.handler.FileDone(done)
		}
	case TypeFileError:
		c.handler.FileError(ParseFileError(fr.Payload))
	}
}

func readFrame(r *bufio.Reader) (Frame, error) {
	var hdr [HeaderLen]byte
	if _, err := io.ReadFull(r, hdr[:]); err != nil {
		return Frame{}, err
	}
	if binary.LittleEndian.Uint32(hdr[0:4]) != Magic ||
		binary.LittleEndian.Uint16(hdr[4:6]) != Version ||
		binary.LittleEndian.Uint16(hdr[10:12]) != 0 {
		return Frame{}, errors.New("bad hc2 header")
	}
	n := binary.LittleEndian.Uint32(hdr[20:24])
	if n > MaxPayload {
		return Frame{}, errors.New("hc2 payload too large")
	}
	payload := make([]byte, int(n))
	if n > 0 {
		if _, err := io.ReadFull(r, payload); err != nil {
			return Frame{}, err
		}
	}
	return Frame{
		Type:      binary.LittleEndian.Uint16(hdr[6:8]),
		Flags:     binary.LittleEndian.Uint16(hdr[8:10]),
		RequestID: binary.LittleEndian.Uint64(hdr[12:20]),
		Payload:   payload,
	}, nil
}

func DecodeError(p []byte) Error {
	e := Error{Code: ErrInternal}
	if len(p) >= 4 {
		e.Code = binary.LittleEndian.Uint32(p[:4])
	}
	if len(p) >= 8 {
		n := int(binary.LittleEndian.Uint32(p[4:8]))
		if n > len(p)-8 {
			n = len(p) - 8
		}
		e.Message = string(p[8 : 8+n])
	}
	return e
}

func DecodeExecState(p []byte) (ExecState, bool) {
	if len(p) < 32 {
		return ExecState{}, false
	}
	return ExecState{
		Ref:          binary.LittleEndian.Uint64(p[:8]),
		TargetExecID: binary.LittleEndian.Uint64(p[8:16]),
		State:        p[16],
		Fail:         p[17],
		Errno:        binary.LittleEndian.Uint16(p[18:20]),
		ExitCode:     binary.LittleEndian.Uint32(p[20:24]),
		TermSignal:   binary.LittleEndian.Uint32(p[24:28]),
		Flags:        binary.LittleEndian.Uint32(p[28:32]),
	}, true
}

func DecodeStatus(p []byte) (Status, error) {
	d := decoder{p: p}
	var st Status
	st.BuildVersion = d.string()
	st.CarrierOpen = d.u8() != 0
	st.CarrierRX = d.u64()
	st.CarrierTX = d.u64()
	st.Outer = StatusOuter{State: d.u8(), LocalConn: d.u64(), PeerConn: d.u64(), CRCErrors: d.u64(), SeqErrors: d.u64(), ResyncDrop: d.u64()}
	st.LMUX = StatusLMUX{State: d.u8(), LocalConn: d.u64(), PeerConn: d.u64(), EstablishedCount: d.u64()}
	st.Factories = Factories{PTY: d.u8() != 0, UnixRendezvous: d.u8() != 0, File: d.u8() != 0, Exec: d.u8() != 0}
	count := d.u32()
	if count > 128 {
		return st, errors.New("too many services")
	}
	for i := uint32(0); i < count; i++ {
		st.Services = append(st.Services, ServiceRow{
			Name:         d.string(),
			Type:         d.string(),
			Acknowledged: d.u8() != 0,
			ServiceID:    d.u64(),
			Internal:     d.u8() != 0,
		})
	}
	if d.err != nil {
		return st, d.err
	}
	return st, nil
}

func EncodeString(buf *bytes.Buffer, s string) error {
	if len(s) > 65535 {
		return errors.New("string too long")
	}
	var b [2]byte
	binary.LittleEndian.PutUint16(b[:], uint16(len(s)))
	buf.Write(b[:])
	buf.WriteString(s)
	return nil
}

func EncodeExecStart(cwd string, argv []string) ([]byte, error) {
	if len(argv) == 0 || len(argv) > 16 {
		return nil, errors.New("bad argv count")
	}
	var b bytes.Buffer
	if err := EncodeString(&b, cwd); err != nil {
		return nil, err
	}
	var n [2]byte
	binary.LittleEndian.PutUint16(n[:], uint16(len(argv)))
	b.Write(n[:])
	for _, arg := range argv {
		if arg == "" {
			return nil, errors.New("empty argv")
		}
		if err := EncodeString(&b, arg); err != nil {
			return nil, err
		}
	}
	if b.Len() > MaxPayload {
		return nil, errors.New("exec start too large")
	}
	return b.Bytes(), nil
}

func EncodeTwoStrings(a, b string) ([]byte, error) {
	var buf bytes.Buffer
	if err := EncodeString(&buf, a); err != nil {
		return nil, err
	}
	if err := EncodeString(&buf, b); err != nil {
		return nil, err
	}
	if buf.Len() > MaxPayload {
		return nil, errors.New("request too large")
	}
	return buf.Bytes(), nil
}

func U64Payload(v uint64) []byte {
	var p [8]byte
	binary.LittleEndian.PutUint64(p[:], v)
	return p[:]
}

func StdinPayload(ref uint64, data []byte) ([]byte, error) {
	if len(data)+8 > MaxPayload {
		return nil, errors.New("stdin frame too large")
	}
	p := make([]byte, 8+len(data))
	binary.LittleEndian.PutUint64(p[:8], ref)
	copy(p[8:], data)
	return p, nil
}

func SignalPayload(ref uint64, sig byte) []byte {
	p := make([]byte, 9)
	binary.LittleEndian.PutUint64(p[:8], ref)
	p[8] = sig
	return p
}

func ParseFileStarted(p []byte) (FileStarted, bool) {
	if len(p) < 24 {
		return FileStarted{}, false
	}
	return FileStarted{
		OpID:          binary.LittleEndian.Uint64(p[:8]),
		Kind:          p[8],
		ExpectedBytes: binary.LittleEndian.Uint64(p[16:24]),
	}, true
}

func ParseFileDone(p []byte) (FileDone, bool) {
	if len(p) < 48 {
		return FileDone{}, false
	}
	var d FileDone
	d.OpID = binary.LittleEndian.Uint64(p[:8])
	d.Bytes = binary.LittleEndian.Uint64(p[8:16])
	copy(d.SHA256[:], p[16:48])
	return d, true
}

func ParseFileError(p []byte) FileError {
	var e FileError
	if len(p) >= 8 {
		e.OpID = binary.LittleEndian.Uint64(p[:8])
	}
	if len(p) >= 12 {
		e.Code = binary.LittleEndian.Uint32(p[8:12])
	}
	if len(p) >= 14 {
		n := int(binary.LittleEndian.Uint16(p[12:14]))
		if n > len(p)-14 {
			n = len(p) - 14
		}
		e.Message = string(p[14 : 14+n])
	}
	return e
}

func SocketExists(path string) bool {
	st, err := os.Stat(path)
	return err == nil && st.Mode()&os.ModeSocket != 0
}

func WithTimeout(parent context.Context, d time.Duration) (context.Context, context.CancelFunc) {
	if d <= 0 {
		return context.WithCancel(parent)
	}
	return context.WithTimeout(parent, d)
}

type decoder struct {
	p   []byte
	off int
	err error
}

func (d *decoder) need(n int) bool {
	if d.err != nil {
		return false
	}
	if len(d.p)-d.off < n {
		d.err = io.ErrUnexpectedEOF
		return false
	}
	return true
}

func (d *decoder) u8() byte {
	if !d.need(1) {
		return 0
	}
	v := d.p[d.off]
	d.off++
	return v
}

func (d *decoder) u32() uint32 {
	if !d.need(4) {
		return 0
	}
	v := binary.LittleEndian.Uint32(d.p[d.off : d.off+4])
	d.off += 4
	return v
}

func (d *decoder) u64() uint64 {
	if !d.need(8) {
		return 0
	}
	v := binary.LittleEndian.Uint64(d.p[d.off : d.off+8])
	d.off += 8
	return v
}

func (d *decoder) string() string {
	if !d.need(2) {
		return ""
	}
	n := int(binary.LittleEndian.Uint16(d.p[d.off : d.off+2]))
	d.off += 2
	if !d.need(n) {
		return ""
	}
	s := string(d.p[d.off : d.off+n])
	d.off += n
	return s
}
