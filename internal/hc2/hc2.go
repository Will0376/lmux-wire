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
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"time"
)

const (
	Magic      uint32 = 0x3243484c
	Version    uint16 = 1
	HeaderLen         = 24
	MaxPayload        = 4096

	TypeClientHello            uint16 = 1
	TypeServerHello            uint16 = 2
	TypeError                  uint16 = 3
	TypeStatusRequest          uint16 = 10
	TypeStatusReply            uint16 = 11
	TypeStatusDetailRequest    uint16 = 12
	TypeStatusDetailReply      uint16 = 13
	TypeExecStart              uint16 = 20
	TypeExecStarted            uint16 = 21
	TypeExecAttach             uint16 = 22
	TypeExecStatus             uint16 = 23
	TypeExecStdinData          uint16 = 24
	TypeExecStdinClose         uint16 = 25
	TypeExecSignal             uint16 = 26
	TypeExecRelease            uint16 = 27
	TypeExecStdout             uint16 = 40
	TypeExecStderr             uint16 = 41
	TypeExecState              uint16 = 42
	TypeExecLost               uint16 = 43
	TypeExecStdinReady         uint16 = 44
	TypeFilePutStart           uint16 = 60
	TypeFileGetStart           uint16 = 61
	TypeFileStarted            uint16 = 62
	TypeFileDone               uint16 = 63
	TypeFileError              uint16 = 64
	TypeFileProgress           uint16 = 65
	TypeFileStatus             uint16 = 66
	TypeFileCancel             uint16 = 67
	TypeFileState              uint16 = 68
	TypeFilePutStartIdempotent uint16 = 69
	TypeFileGetStartIdempotent uint16 = 70
	TypeFileStatusToken        uint16 = 71

	CapStatus        uint32 = 1 << 0
	CapExec          uint32 = 1 << 1
	CapFile          uint32 = 1 << 2
	CapFileLifecycle uint32 = 1 << 3
	CapStatusDetail  uint32 = 1 << 4

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

	FileRunning   byte = 1
	FileDoneState byte = 2
	FileFailed    byte = 3
	FileCancelled byte = 4
	FileLost      byte = 5

	OuterStateEstablished byte = 2
	LMUXStateEstablished  byte = 1
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
	BuildVersion  string              `json:"build_version"`
	CarrierOpen   bool                `json:"carrier_open"`
	CarrierRX     uint64              `json:"carrier_rx_bytes"`
	CarrierTX     uint64              `json:"carrier_tx_bytes"`
	Outer         StatusOuter         `json:"outer"`
	LMUX          StatusLMUX          `json:"lmux"`
	Factories     Factories           `json:"factories"`
	Services      []ServiceRow        `json:"services"`
	Observability StatusObservability `json:"observability"`
}

type StatusObservability struct {
	Supported         bool                  `json:"supported"`
	UnavailableReason string                `json:"unavailable_reason,omitempty"`
	Carrier           *StatusCarrier        `json:"carrier,omitempty"`
	Endpoints         []StatusEndpoint      `json:"endpoints,omitempty"`
	Connection        *StatusConnection     `json:"connection,omitempty"`
	StructuredExec    *StatusStructuredExec `json:"structured_exec,omitempty"`
	Dirty             *StatusDirty          `json:"dirty,omitempty"`
}

type StatusCarrier struct {
	Name                 string `json:"name"`
	Backend              string `json:"backend"`
	Mode                 string `json:"mode"`
	Path                 string `json:"path"`
	RuntimeState         string `json:"runtime_state"`
	FDOpen               bool   `json:"fd_open"`
	FDOwnership          string `json:"fd_ownership"`
	Generation           string `json:"generation"`
	SwitchPhase          string `json:"switch_phase"`
	SwitchTarget         string `json:"switch_target,omitempty"`
	AdministrativePhase  string `json:"administrative_phase"`
	AdministrativeName   string `json:"administrative_name,omitempty"`
	AdministrativeAction string `json:"administrative_action,omitempty"`
	CommittedButDown     bool   `json:"committed_but_down"`
	ProtocolFailed       bool   `json:"protocol_failed"`
	ConsoleReleased      bool   `json:"console_released"`
	CurrentFailure       string `json:"current_failure,omitempty"`
	HistoricalError      string `json:"historical_last_error,omitempty"`
}

type StatusEndpoint struct {
	Name                     string `json:"name"`
	Backend                  string `json:"backend"`
	ConfiguredMode           string `json:"configured_mode"`
	CurrentMode              string `json:"current_mode"`
	Role                     string `json:"role"`
	RuntimeState             string `json:"runtime_state"`
	EffectiveState           string `json:"effective_state"`
	AdministrativelyReleased bool   `json:"administratively_released"`
	TargetConfirmation       string `json:"target_administrative_confirmation"`
	Path                     string `json:"path"`
	FDOpen                   bool   `json:"fd_open"`
	FDOwnership              string `json:"fd_ownership"`
	CommittedButDown         bool   `json:"committed_but_down"`
	ProtocolFailed           bool   `json:"protocol_failed"`
	ConsoleReleased          bool   `json:"console_released"`
	HistoricalErrno          int32  `json:"historical_errno"`
}

type StatusConnection struct {
	Overall         string `json:"overall"`
	OuterState      string `json:"outer_state"`
	LMUXState       string `json:"lmux_state"`
	OuterLocalID    string `json:"outer_local_id"`
	OuterPeerID     string `json:"outer_peer_id"`
	LMUXLocalID     string `json:"lmux_local_id"`
	LMUXPeerID      string `json:"lmux_peer_id"`
	CurrentFailure  string `json:"current_failure,omitempty"`
	ServicesReady   uint32 `json:"services_ready"`
	ServicesDesired uint32 `json:"services_desired"`
}

type StatusStructuredExec struct {
	ActiveControlOperation string `json:"active_control_operation"`
	PendingQueue           uint32 `json:"pending_queue"`
	RetiringControlStream  string `json:"retiring_control_stream"`
	CompletedTransactions  string `json:"completed_transactions"`
	TotalTransactions      string `json:"total_transactions"`
	AutoStatusCount        string `json:"auto_status_count"`
	Failure                string `json:"failure,omitempty"`
}

type StatusDirtyCounters struct {
	InvalidRecords   string `json:"invalid_records"`
	ForeignBytes     string `json:"foreign_bytes"`
	CRCFailures      string `json:"crc_failures"`
	Retransmissions  string `json:"retransmissions"`
	RetryFailures    string `json:"retry_failures"`
	DeadlineFailures string `json:"deadline_failures"`
}

type StatusDirty struct {
	Active            bool                 `json:"active"`
	CurrentFailure    bool                 `json:"current_failure"`
	LastFailure       string               `json:"historical_last_failure,omitempty"`
	Session           *StatusDirtyCounters `json:"session,omitempty"`
	LifetimeAvailable bool                 `json:"lifetime_available"`
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
	Incarnation   uint64
}

type FileOperation struct {
	OpID        uint64
	LocalConn   uint64
	PeerConn    uint64
	Incarnation uint64
	Bytes       uint64
	Total       uint64
	ErrorCode   uint32
	Kind        byte
	State       byte
	TotalKnown  bool
	SHA256      [32]byte
	Error       string
	LocalPath   string
	TargetPath  string
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

func (c *Client) Closed() (bool, error) {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.closed, c.err
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
		_ = c.conn.Close()
		c.closeWith(err)
		return Frame{}, err
	}
	for {
		select {
		case <-ctx.Done():
			c.forget(req)
			return Frame{}, ctx.Err()
		case fr, ok := <-ch:
			if !ok {
				_, err := c.Closed()
				if err == nil {
					err = io.ErrClosedPipe
				}
				return Frame{}, err
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

func UnsupportedObservability(reason string) StatusObservability {
	return StatusObservability{Supported: false, UnavailableReason: reason}
}

func DecodeStatusDetail(p []byte) (StatusObservability, error) {
	d := decoder{p: p}
	if version := d.u16(); version != 1 {
		if d.err != nil {
			return StatusObservability{}, d.err
		}
		return StatusObservability{}, fmt.Errorf("unsupported status detail version %d", version)
	}
	toString := func(v uint64) string { return strconv.FormatUint(v, 10) }
	o := StatusObservability{Supported: true}
	o.Carrier = &StatusCarrier{
		Name: d.string(), Backend: d.string(), Mode: d.string(), Path: d.string(),
		RuntimeState: d.string(), FDOpen: d.u8() != 0, FDOwnership: d.string(),
		Generation: toString(d.u64()), SwitchPhase: d.string(),
		SwitchTarget: d.string(), AdministrativePhase: d.string(),
		AdministrativeName: d.string(), AdministrativeAction: d.string(),
		CommittedButDown: d.u8() != 0, ProtocolFailed: d.u8() != 0,
		ConsoleReleased: d.u8() != 0, CurrentFailure: d.string(),
		HistoricalError: d.string(),
	}
	count := d.u8()
	if count > 16 {
		return StatusObservability{}, errors.New("too many status endpoints")
	}
	for i := byte(0); i < count; i++ {
		o.Endpoints = append(o.Endpoints, StatusEndpoint{
			Name: d.string(), Backend: d.string(), ConfiguredMode: d.string(),
			CurrentMode: d.string(), Role: d.string(), RuntimeState: d.string(),
			EffectiveState: d.string(), AdministrativelyReleased: d.u8() != 0,
			TargetConfirmation: d.string(), Path: d.string(), FDOpen: d.u8() != 0,
			FDOwnership: d.string(), CommittedButDown: d.u8() != 0,
			ProtocolFailed: d.u8() != 0, ConsoleReleased: d.u8() != 0,
			HistoricalErrno: int32(d.u32()),
		})
	}
	o.Connection = &StatusConnection{
		Overall: d.string(), OuterState: d.string(), LMUXState: d.string(),
		OuterLocalID: toString(d.u64()), OuterPeerID: toString(d.u64()),
		LMUXLocalID: toString(d.u64()), LMUXPeerID: toString(d.u64()),
		CurrentFailure: d.string(), ServicesReady: d.u32(),
		ServicesDesired: d.u32(),
	}
	o.StructuredExec = &StatusStructuredExec{
		ActiveControlOperation: d.string(), PendingQueue: d.u32(),
		RetiringControlStream: toString(d.u64()),
		CompletedTransactions: toString(d.u64()),
		TotalTransactions:     toString(d.u64()), AutoStatusCount: toString(d.u64()),
		Failure: d.string(),
	}
	dirty := &StatusDirty{Active: d.u8() != 0}
	if dirty.Active {
		dirty.CurrentFailure = d.u8() != 0
		dirty.LastFailure = d.string()
		dirty.Session = &StatusDirtyCounters{
			InvalidRecords: toString(d.u64()), ForeignBytes: toString(d.u64()),
			CRCFailures: toString(d.u64()), Retransmissions: toString(d.u64()),
			RetryFailures: toString(d.u64()), DeadlineFailures: toString(d.u64()),
		}
	}
	// DIRTY counters are reset with each adapter session. The host does not
	// aggregate a distinct process-lifetime DIRTY total.
	dirty.LifetimeAvailable = false
	o.Dirty = dirty
	if d.err != nil {
		return StatusObservability{}, d.err
	}
	return o, nil
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

func EncodeFileStart(requestNamespace, requestToken, a, b string) ([]byte, error) {
	var buf bytes.Buffer
	for _, s := range []string{requestNamespace, requestToken, a, b} {
		if s == "" || strings.IndexByte(s, 0) >= 0 {
			return nil, errors.New("FILE request fields must be non-empty and contain no NUL")
		}
		if err := EncodeString(&buf, s); err != nil {
			return nil, err
		}
	}
	if len(requestNamespace) >= 64 || len(requestToken) >= 128 {
		return nil, errors.New("FILE request namespace or token too long")
	}
	if buf.Len() > MaxPayload {
		return nil, errors.New("FILE start request too large")
	}
	return buf.Bytes(), nil
}

func EncodeFileToken(requestNamespace, requestToken string) ([]byte, error) {
	var buf bytes.Buffer
	for _, s := range []string{requestNamespace, requestToken} {
		if s == "" || strings.IndexByte(s, 0) >= 0 {
			return nil, errors.New("FILE request namespace and token are required")
		}
		if err := EncodeString(&buf, s); err != nil {
			return nil, err
		}
	}
	if len(requestNamespace) >= 64 || len(requestToken) >= 128 {
		return nil, errors.New("FILE request namespace or token too long")
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
	s := FileStarted{
		OpID:          binary.LittleEndian.Uint64(p[:8]),
		Kind:          p[8],
		ExpectedBytes: binary.LittleEndian.Uint64(p[16:24]),
	}
	if len(p) >= 32 {
		s.Incarnation = binary.LittleEndian.Uint64(p[24:32])
	}
	return s, true
}

func ParseFileOperation(p []byte) (FileOperation, bool) {
	if len(p) < 96 {
		return FileOperation{}, false
	}
	var op FileOperation
	op.OpID = binary.LittleEndian.Uint64(p[0:8])
	op.LocalConn = binary.LittleEndian.Uint64(p[8:16])
	op.PeerConn = binary.LittleEndian.Uint64(p[16:24])
	op.Incarnation = binary.LittleEndian.Uint64(p[24:32])
	op.Bytes = binary.LittleEndian.Uint64(p[32:40])
	op.Total = binary.LittleEndian.Uint64(p[40:48])
	op.ErrorCode = binary.LittleEndian.Uint32(p[48:52])
	op.Kind = p[52]
	op.State = p[53]
	op.TotalKnown = p[54] != 0
	copy(op.SHA256[:], p[64:96])
	errorLen := int(binary.LittleEndian.Uint16(p[56:58]))
	localLen := int(binary.LittleEndian.Uint16(p[58:60]))
	targetLen := int(binary.LittleEndian.Uint16(p[60:62]))
	if errorLen+localLen+targetLen > len(p)-96 {
		return FileOperation{}, false
	}
	off := 96
	op.Error = string(p[off : off+errorLen])
	off += errorLen
	op.LocalPath = string(p[off : off+localLen])
	off += localLen
	op.TargetPath = string(p[off : off+targetLen])
	return op, op.OpID != 0
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

func (d *decoder) u16() uint16 {
	if !d.need(2) {
		return 0
	}
	v := binary.LittleEndian.Uint16(d.p[d.off : d.off+2])
	d.off += 2
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
