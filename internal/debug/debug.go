package debug

import (
	"context"
	"encoding/base64"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"time"

	"lmux/internal/gdbmi"
	"lmux/internal/hc2"
)

const (
	DefaultMaxSessions             = 2
	DefaultGDBRemoteTimeoutSeconds = 10
	TargetExecCapacity             = 4
	MaxRSPChunk                    = hc2.MaxPayload - 8
	MaxRSPQueueChunks              = 64
	MaxRSPPendingBytes             = MaxRSPChunk * MaxRSPQueueChunks
	MaxMemoryRead                  = 64 * 1024
)

type HC2 interface {
	Request(context.Context, uint16, []byte) (hc2.Frame, error)
}

type JournalFunc func(string, map[string]any)

type Config struct {
	RuntimeDir              string
	GDBPath                 string
	TargetGDBServer         string
	MaxSessions             int
	Sysroot                 string
	SolibSearchPath         string
	GDBRemoteTimeoutSeconds int
	TargetExecCap           int
	CommandActiveFunc       func() int
}

type Manager struct {
	cfg     Config
	hc      HC2
	journal JournalFunc

	mu       sync.Mutex
	next     uint64
	sessions map[string]*Session
	byRef    map[uint64]*Session
}

type Session struct {
	mgr *Manager

	Token       string
	Label       string
	Mode        string
	RemoteDebug string
	Symbols     string
	PID         int
	Program     string
	Args        []string

	State           string
	StopReason      string
	ThreadID        string
	Frame           map[string]any
	LastError       string
	InferiorCleanup string

	GDBPID  int
	ExecRef uint64
	Arch    string

	socketPath string
	ln         net.Listener
	conn       net.Conn
	gdb        *gdbmi.Process

	connectCancel context.CancelFunc

	acceptAttempted bool
	acceptSucceeded bool
	acceptedAt      time.Time
	acceptError     string
	rspLocalClose   string

	toGDB     chan []byte
	connReady chan struct{}
	done      chan struct{}
	execDone  chan struct{}
	wait      chan struct{}

	targetDiag tail
	hostMI     tail
	rspStats   rspStats
	rspTrace   rspTrace

	rspMu       sync.Mutex
	rspPending  []byte
	rspData     chan struct{}
	rspSpace    chan struct{}
	stdinReady  chan struct{}
	readyGen    uint64
	cleaned     bool
	tombstoneAt time.Time
	cleanupMu   sync.Mutex
	mu          sync.Mutex
}

type rspStats struct {
	GDBRxBytes                   uint64
	TargetTxBytes                uint64
	TargetRxBytes                uint64
	GDBTxBytes                   uint64
	TargetStdinBackpressureCount uint64
	TargetStdinReadyCount        uint64
	PendingToGDBBytes            uint64
	LastProgress                 time.Time
}

const maxRSPTraceBytesPerDirection = 32 * 1024

type rspTrace struct {
	G2TSeq   uint64
	T2GSeq   uint64
	G2TBytes int
	T2GBytes int
	Entries  []rspTraceEntry
}

type rspTraceEntry struct {
	Direction string `json:"direction"`
	Seq       uint64 `json:"seq"`
	Len       int    `json:"len"`
	Dropped   bool   `json:"dropped,omitempty"`
	Hex       string `json:"hex,omitempty"`
	Escaped   string `json:"escaped,omitempty"`
}

type tail struct {
	max int
	buf []byte
}

func (t *tail) add(p []byte) {
	if t.max <= 0 {
		t.max = 64 * 1024
	}
	if len(p) >= t.max {
		t.buf = append(t.buf[:0], p[len(p)-t.max:]...)
		return
	}
	t.buf = append(t.buf, p...)
	if len(t.buf) > t.max {
		copy(t.buf, t.buf[len(t.buf)-t.max:])
		t.buf = t.buf[:t.max]
	}
}

func (t *tail) string() string { return string(t.buf) }
func (t *tail) len() int       { return len(t.buf) }

type StartRequest struct {
	Mode        string
	Symbols     string
	PID         int
	Program     string
	Args        []string
	Label       string
	RemoteDebug string
}

type DiagnosticsRequest struct {
	Source    string
	Cursor    string
	MaxBytes  int
	MaxEvents int
	Contains  string
}

type diagCursor struct {
	Source string `json:"source"`
	Pos    int    `json:"pos"`
}

func NewManager(cfg Config, hc HC2, journal JournalFunc) *Manager {
	if cfg.MaxSessions <= 0 {
		cfg.MaxSessions = DefaultMaxSessions
	}
	if cfg.TargetExecCap <= 0 {
		cfg.TargetExecCap = TargetExecCapacity
	}
	if cfg.GDBRemoteTimeoutSeconds == 0 {
		cfg.GDBRemoteTimeoutSeconds = DefaultGDBRemoteTimeoutSeconds
	}
	if cfg.MaxSessions > cfg.TargetExecCap {
		cfg.MaxSessions = cfg.TargetExecCap
	}
	return &Manager{cfg: cfg, hc: hc, journal: journal, sessions: map[string]*Session{}, byRef: map[uint64]*Session{}}
}

func (m *Manager) Capabilities() map[string]any {
	active := m.ActiveCount()
	gdbPath := m.cfg.GDBPath
	if gdbPath == "" {
		gdbPath = "arm-linux-gnueabi-gdb"
	}
	return map[string]any{
		"configured":                   m.cfg.TargetGDBServer != "",
		"gdb_path":                     gdbPath,
		"target_gdbserver":             m.cfg.TargetGDBServer,
		"gdb_remote_timeout_seconds":   m.cfg.GDBRemoteTimeoutSeconds,
		"mi_async":                     true,
		"interrupt_transport":          "gdb-rsp",
		"interrupt_stop_requires_wait": true,
		"max_debug_sessions":           m.cfg.MaxSessions,
		"active_debug_sessions":        active,
		"target_exec_capacity":         m.cfg.TargetExecCap,
		"target_exec_slots_in_use":     active + m.commandActive(),
		"supported_modes":              []string{"attach", "launch"},
		"self_debug_lmux_supported":    false,
		"self_debug_lmux_restriction":  "debugging the LMUX worker/supervisor carrying this transport is unsupported",
	}
}

func (m *Manager) commandActive() int {
	if m.cfg.CommandActiveFunc == nil {
		return 0
	}
	return m.cfg.CommandActiveFunc()
}

func (m *Manager) ActiveCount() int {
	m.mu.Lock()
	defer m.mu.Unlock()
	return m.activeCountLocked()
}

func (m *Manager) activeCountLocked() int {
	active := 0
	for _, s := range m.sessions {
		if s.isActive() {
			active++
		}
	}
	return active
}

func (m *Manager) List() []map[string]any {
	m.mu.Lock()
	defer m.mu.Unlock()
	out := make([]map[string]any, 0, len(m.sessions))
	for _, s := range m.sessions {
		out = append(out, s.summary())
	}
	return out
}

func (m *Manager) StopAll(ctx context.Context) {
	m.mu.Lock()
	sessions := make([]*Session, 0, len(m.sessions))
	for _, s := range m.sessions {
		sessions = append(sessions, s)
	}
	m.mu.Unlock()
	for _, s := range sessions {
		_ = s.Stop(ctx)
	}
}

func (m *Manager) Start(ctx context.Context, req StartRequest) (*Session, error) {
	if m.cfg.TargetGDBServer == "" {
		return nil, errors.New("NOT_CONFIGURED: --target-gdbserver is required")
	}
	remoteDebug := req.RemoteDebug
	if remoteDebug == "" {
		remoteDebug = "off"
	}
	switch remoteDebug {
	case "off", "gdbserver", "gdb", "both":
	default:
		return nil, errors.New("INVALID_ARGUMENT: remote_debug must be off, gdbserver, gdb, or both")
	}
	if req.Symbols == "" || !filepath.IsAbs(req.Symbols) {
		return nil, errors.New("INVALID_ARGUMENT: symbols must be an absolute host path")
	}
	if m.cfg.GDBRemoteTimeoutSeconds < 1 || m.cfg.GDBRemoteTimeoutSeconds > 300 {
		return nil, errors.New("INVALID_ARGUMENT: gdb remote timeout must be in range 1..300 seconds")
	}
	if st, err := os.Stat(req.Symbols); err != nil || st.IsDir() {
		return nil, errors.New("SYMBOLS_NOT_FOUND")
	}
	switch req.Mode {
	case "attach":
		if req.PID <= 0 || req.Program != "" || len(req.Args) != 0 {
			return nil, errors.New("INVALID_ARGUMENT: attach requires pid only")
		}
	case "launch":
		if req.Program == "" || !filepath.IsAbs(req.Program) || req.PID != 0 {
			return nil, errors.New("INVALID_ARGUMENT: launch requires absolute program")
		}
	default:
		return nil, errors.New("INVALID_ARGUMENT: mode must be attach or launch")
	}
	m.mu.Lock()
	if m.activeCountLocked() >= m.cfg.MaxSessions || m.activeCountLocked()+m.commandActive() >= m.cfg.TargetExecCap {
		m.mu.Unlock()
		return nil, errors.New("RESOURCE_LIMIT")
	}
	m.next++
	token := fmt.Sprintf("d-%d-%s", m.next, randHex(m.next))
	runtimeDir := m.cfg.RuntimeDir
	socketDir := filepath.Join(runtimeDir, "debug")
	if err := os.MkdirAll(socketDir, 0700); err != nil {
		m.mu.Unlock()
		return nil, err
	}
	s := &Session{mgr: m, Token: token, Label: req.Label, Mode: req.Mode, RemoteDebug: remoteDebug, Symbols: req.Symbols, PID: req.PID, Program: req.Program, Args: append([]string(nil), req.Args...), State: "STARTING", socketPath: filepath.Join(socketDir, token+".sock"), toGDB: make(chan []byte, MaxRSPQueueChunks), connReady: make(chan struct{}), done: make(chan struct{}), execDone: make(chan struct{}), wait: make(chan struct{}), rspData: make(chan struct{}, 1), rspSpace: make(chan struct{}, 1), stdinReady: make(chan struct{}, 1)}
	m.sessions[token] = s
	m.mu.Unlock()
	if m.journal != nil {
		m.journal("DEBUG_START", map[string]any{"debug": token, "mode": req.Mode, "label": req.Label})
	}
	if err := s.start(ctx); err != nil {
		s.fail("ERROR", err)
		s.cleanup(context.Background(), true, true)
		return nil, err
	}
	return s, nil
}

func (s *Session) start(ctx context.Context) error {
	s.ensureRSPChannels()
	_ = os.Remove(s.socketPath)
	ln, err := net.Listen("unix", s.socketPath)
	if err != nil {
		return err
	}
	if uln, ok := ln.(*net.UnixListener); ok {
		uln.SetUnlinkOnClose(false)
	}
	if err := os.Chmod(s.socketPath, 0600); err != nil {
		ln.Close()
		_ = os.Remove(s.socketPath)
		return err
	}
	s.ln = ln
	if err := s.validateRSPSocketPath("RSP_SOCKET_LISTEN"); err != nil {
		ln.Close()
		_ = os.Remove(s.socketPath)
		return err
	}
	argv := []string{s.mgr.cfg.TargetGDBServer}
	if s.RemoteDebug == "gdbserver" || s.RemoteDebug == "both" {
		argv = append(argv, "--remote-debug")
	}
	if s.Mode == "attach" {
		argv = append(argv, "--attach", "stdio", strconv.Itoa(s.PID))
	} else {
		argv = append(argv, "stdio", s.Program)
		argv = append(argv, s.Args...)
	}
	p, err := hc2.EncodeExecStart("", argv)
	if err != nil {
		return err
	}
	cctx, cancel := context.WithTimeout(ctx, 15*time.Second)
	defer cancel()
	fr, err := s.mgr.hc.Request(cctx, hc2.TypeExecStart, p)
	if err != nil {
		var hc2Err *hc2.Error
		if errors.As(err, &hc2Err) && hc2Err.Code == hc2.ErrNotFound {
			return errors.New("GDBSERVER_NOT_FOUND")
		}
		return fmt.Errorf("GDBSERVER_START_FAILED: %w", err)
	}
	st, ok := hc2.DecodeExecState(fr.Payload)
	if !ok || fr.Type != hc2.TypeExecStarted || st.Ref == 0 {
		return errors.New("GDBSERVER_START_FAILED")
	}
	s.mu.Lock()
	s.ExecRef = st.Ref
	s.State = "CONNECTING"
	s.mu.Unlock()
	s.mgr.mu.Lock()
	s.mgr.byRef[st.Ref] = s
	s.mgr.mu.Unlock()
	go s.acceptLoop()
	go s.writeTargetStdin()
	go s.writeToGDB()
	gdbPath := s.mgr.cfg.GDBPath
	if gdbPath == "" {
		gdbPath = "arm-linux-gnueabi-gdb"
	}
	gdb, err := gdbmi.StartGDB(context.Background(), gdbPath)
	if err != nil {
		return fmt.Errorf("GDB_NOT_FOUND: %w", err)
	}
	s.gdb = gdb
	s.GDBPID = gdb.PID()
	go s.gdbEvents()
	if _, err := gdb.Command(ctx, "-file-exec-and-symbols "+miQuote(s.Symbols)); err != nil {
		return s.startFailure("GDB_SYMBOLS_FAILED", err)
	}
	if s.mgr.cfg.Sysroot != "" {
		if _, err := gdb.Command(ctx, "-gdb-set sysroot "+miQuote(s.mgr.cfg.Sysroot)); err != nil {
			return s.startFailure("GDB_SYSROOT_FAILED", err)
		}
	}
	if s.mgr.cfg.SolibSearchPath != "" {
		if _, err := gdb.Command(ctx, "-gdb-set solib-search-path "+miQuote(s.mgr.cfg.SolibSearchPath)); err != nil {
			return s.startFailure("GDB_SOLIB_SEARCH_PATH_FAILED", err)
		}
	}
	if _, err := gdb.Command(ctx, "-interpreter-exec console "+miQuote(fmt.Sprintf("set remotetimeout %d", s.mgr.cfg.GDBRemoteTimeoutSeconds))); err != nil {
		return s.startFailure("GDB_REMOTE_TIMEOUT_FAILED", err)
	}
	if _, err := gdb.Command(ctx, "-gdb-set mi-async on"); err != nil {
		return s.startFailure("GDB_MI_ASYNC_FAILED", err)
	}
	if s.RemoteDebug == "gdb" || s.RemoteDebug == "both" {
		if _, err := gdb.Command(ctx, "-interpreter-exec console "+miQuote("set debug remote 1")); err != nil {
			return s.startFailure("GDB_REMOTE_DEBUG_FAILED", err)
		}
	}
	connectCtx, cancel := context.WithCancel(context.Background())
	s.mu.Lock()
	s.connectCancel = cancel
	s.mu.Unlock()
	go s.runTargetSelect(connectCtx)
	return nil
}

func (s *Session) runTargetSelect(ctx context.Context) {
	beforeTargetSelect := s.socketSnapshot("before")
	if err := s.validateRSPSocketPath("RSP_SOCKET_PRE_TARGET_SELECT"); err != nil {
		s.fail("ERROR", err)
		return
	}
	if _, err := s.gdb.CommandNoDefaultTimeout(ctx, "-target-select remote "+s.socketPath); err != nil {
		select {
		case <-s.done:
			return
		default:
		}
		if ctx.Err() != nil {
			s.fail("ERROR", s.targetSelectFailure(ctx.Err(), beforeTargetSelect))
			return
		}
		s.fail("ERROR", s.targetSelectFailure(err, beforeTargetSelect))
		return
	}
	if err := s.currentFailure(); err != nil {
		return
	}
	s.setState("STOPPED", "")
	if s.mgr.journal != nil {
		s.mgr.journal("DEBUG_CONNECTED", map[string]any{"debug": s.Token})
	}
}

func (s *Session) ensureRSPChannels() {
	s.rspMu.Lock()
	if s.rspData == nil {
		s.rspData = make(chan struct{}, 1)
	}
	if s.rspSpace == nil {
		s.rspSpace = make(chan struct{}, 1)
	}
	if s.stdinReady == nil {
		s.stdinReady = make(chan struct{}, 1)
	}
	s.rspMu.Unlock()
}

func (m *Manager) ExecOutput(stream string, ref uint64, data []byte) bool {
	m.mu.Lock()
	s := m.byRef[ref]
	m.mu.Unlock()
	if s == nil {
		return false
	}
	if stream == "stderr" {
		s.mu.Lock()
		s.targetDiag.add(data)
		s.mu.Unlock()
		return true
	}
	chunk := append([]byte(nil), data...)
	s.addTargetRx(len(chunk))
	select {
	case s.toGDB <- chunk:
		return true
	case <-s.done:
		s.dropPendingToGDB(len(chunk))
		return true
	}
}

func (m *Manager) ExecState(st hc2.ExecState) bool {
	m.mu.Lock()
	s := m.byRef[st.Ref]
	m.mu.Unlock()
	if s == nil {
		return false
	}
	if st.State == 4 || st.State == 5 {
		closeOnce(s.execDone)
		s.fail("LOST", fmt.Errorf("GDBSERVER_EXITED: state=%s exit=%d signal=%d fail=%d errno=%d stderr=%q",
			execStateName(st.State), st.ExitCode, st.TermSignal, st.Fail, st.Errno, s.targetStderrTail()))
	}
	return true
}

func (m *Manager) ExecStdinReady(ref uint64) bool {
	m.mu.Lock()
	s := m.byRef[ref]
	m.mu.Unlock()
	if s == nil {
		return false
	}
	s.notifyStdinReady()
	return true
}

func (m *Manager) ExecLost(ref uint64, reason uint32) bool {
	m.mu.Lock()
	s := m.byRef[ref]
	m.mu.Unlock()
	if s == nil {
		return false
	}
	closeOnce(s.execDone)
	s.fail("LOST", fmt.Errorf("GDBSERVER_LOST: reason=%d stderr=%q", reason, s.targetStderrTail()))
	return true
}

func (m *Manager) Get(token string) (*Session, bool) {
	m.mu.Lock()
	defer m.mu.Unlock()
	s, ok := m.sessions[token]
	return s, ok
}

func (s *Session) startFailure(stage string, err error) error {
	if first := s.currentFailure(); first != nil {
		return first
	}
	msg := fmt.Sprintf("%s: %v", stage, err)
	if s.gdb != nil {
		if diag := s.gdb.Diagnostics(); diag != "" {
			msg += fmt.Sprintf(` gdb_stderr=%q`, diag)
		}
	}
	if diag := s.targetStderrTail(); diag != "" {
		msg += fmt.Sprintf(` target_gdbserver_stderr=%q`, diag)
	}
	if diag := s.hostMITail(); diag != "" {
		msg += fmt.Sprintf(` host_gdb_mi_stream=%q`, diag)
	}
	return errors.New(msg)
}

func (s *Session) targetSelectFailure(err error, before rspSocketSnapshot) error {
	after := s.socketSnapshot("after")
	if strings.Contains(err.Error(), "remote closed") {
		s.mu.Lock()
		if s.acceptSucceeded && s.rspLocalClose == "" {
			s.rspLocalClose = "RSP_LOCAL_CLOSED: gdb unix socket closed"
		}
		s.mu.Unlock()
	}
	first := s.currentFailure()
	if first != nil {
		return fmt.Errorf("%w target_select_error=%q raw_target_select=%q accept=%s socket_before={%s} socket_after={%s} local_rsp_close=%q target_gdbserver_stderr=%q host_gdb_mi_stream=%q",
			first, err.Error(), string(s.rawTargetSelectBytes()), s.acceptSummary(), before.String(), after.String(), s.localRSPClose(), s.targetStderrTail(), s.hostMITail())
	}
	prefix := "TARGET_SELECT_FAILED"
	if before.Exists && !after.Exists {
		prefix = "RSP_SOCKET_DISAPPEARED_DURING_TARGET_SELECT"
	}
	msg := fmt.Sprintf("%s: %v raw_target_select=%q accept=%s socket_before={%s} socket_after={%s}",
		prefix, err, string(s.rawTargetSelectBytes()), s.acceptSummary(), before.String(), after.String())
	if local := s.localRSPClose(); local != "" {
		msg += fmt.Sprintf(` local_rsp_close=%q`, local)
	}
	if s.gdb != nil {
		if diag := s.gdb.Diagnostics(); diag != "" {
			msg += fmt.Sprintf(` gdb_stderr=%q`, diag)
		}
	}
	if diag := s.targetStderrTail(); diag != "" {
		msg += fmt.Sprintf(` target_gdbserver_stderr=%q`, diag)
	}
	if diag := s.hostMITail(); diag != "" {
		msg += fmt.Sprintf(` host_gdb_mi_stream=%q`, diag)
	}
	return errors.New(msg)
}

func (s *Session) validateRSPSocketPath(stage string) error {
	snap := s.socketSnapshot(stage)
	if !snap.ListenerValid {
		return fmt.Errorf("RSP_SOCKET_LISTENER_MISSING: %s", snap.String())
	}
	if !snap.Exists {
		return fmt.Errorf("RSP_SOCKET_MISSING: %s", snap.String())
	}
	if !snap.IsSocket {
		return fmt.Errorf("RSP_SOCKET_NOT_SOCKET: %s", snap.String())
	}
	return nil
}

type rspSocketSnapshot struct {
	Stage         string
	Path          string
	Exists        bool
	Mode          string
	IsSocket      bool
	LstatError    string
	ListenerValid bool
}

func (s rspSocketSnapshot) String() string {
	return fmt.Sprintf("stage=%s path=%s exists=%t mode=%s is_socket=%t listener_valid=%t lstat_error=%q",
		s.Stage, s.Path, s.Exists, s.Mode, s.IsSocket, s.ListenerValid, s.LstatError)
}

func (s *Session) socketSnapshot(stage string) rspSocketSnapshot {
	s.mu.Lock()
	ln := s.ln
	s.mu.Unlock()
	snap := rspSocketSnapshot{Stage: stage, Path: s.socketPath, ListenerValid: ln != nil && ln.Addr() != nil}
	st, err := os.Lstat(s.socketPath)
	if err != nil {
		snap.LstatError = err.Error()
		return snap
	}
	snap.Exists = true
	snap.Mode = st.Mode().String()
	snap.IsSocket = st.Mode()&os.ModeSocket != 0
	return snap
}

func (s *Session) rawTargetSelectBytes() []byte {
	if s.gdb == nil {
		return nil
	}
	return s.gdb.LastTargetSelectBytes()
}

func (s *Session) acceptSummary() string {
	s.mu.Lock()
	defer s.mu.Unlock()
	at := ""
	if !s.acceptedAt.IsZero() {
		at = s.acceptedAt.Format(time.RFC3339Nano)
	}
	return fmt.Sprintf("attempted=%t succeeded=%t accepted_at=%q error=%q",
		s.acceptAttempted, s.acceptSucceeded, at, s.acceptError)
}

func (s *Session) inStartup() bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.State == "STARTING" || s.State == "CONNECTING"
}

func (s *Session) currentFailure() error {
	s.mu.Lock()
	state := s.State
	first := s.LastError
	s.mu.Unlock()
	if first != "" && (state == "ERROR" || state == "LOST") {
		return errors.New(first)
	}
	return nil
}

func (s *Session) targetStderrTail() string {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.targetDiag.string()
}

func (s *Session) hostMITail() string {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.hostMI.string()
}

func (s *Session) localRSPClose() string {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.rspLocalClose
}

func (s *Session) Status() map[string]any {
	s.rspMu.Lock()
	pendingToTarget := len(s.rspPending)
	s.rspMu.Unlock()
	s.mu.Lock()
	defer s.mu.Unlock()
	acceptedAt := ""
	if !s.acceptedAt.IsZero() {
		acceptedAt = s.acceptedAt.Format(time.RFC3339Nano)
	}
	lastProgress := ""
	if !s.rspStats.LastProgress.IsZero() {
		lastProgress = s.rspStats.LastProgress.Format(time.RFC3339Nano)
	}
	acceptSummary := fmt.Sprintf("attempted=%t succeeded=%t accepted_at=%q error=%q",
		s.acceptAttempted, s.acceptSucceeded, acceptedAt, s.acceptError)
	return map[string]any{
		"debug_token":                     s.Token,
		"label":                           s.Label,
		"mode":                            s.Mode,
		"remote_debug":                    s.RemoteDebug,
		"gdb_remote_timeout_seconds":      s.mgr.cfg.GDBRemoteTimeoutSeconds,
		"mi_async":                        true,
		"interrupt_transport":             "gdb-rsp",
		"interrupt_stop_requires_wait":    true,
		"state":                           s.State,
		"stop_reason":                     s.StopReason,
		"thread_id":                       s.ThreadID,
		"frame":                           s.Frame,
		"inferior_cleanup":                s.InferiorCleanup,
		"gdb_alive":                       s.gdb != nil && !s.cleaned,
		"gdbserver_alive":                 s.ExecRef != 0 && !s.cleaned && s.State != "LOST" && s.State != "ERROR" && s.State != "DETACHED" && s.State != "EXITED",
		"last_error":                      s.LastError,
		"gdb_pid":                         s.GDBPID,
		"rsp_accept":                      acceptSummary,
		"local_rsp_close":                 s.rspLocalClose,
		"rsp_gdb_rx_bytes":                s.rspStats.GDBRxBytes,
		"rsp_target_tx_bytes":             s.rspStats.TargetTxBytes,
		"rsp_target_rx_bytes":             s.rspStats.TargetRxBytes,
		"rsp_gdb_tx_bytes":                s.rspStats.GDBTxBytes,
		"target_stdin_backpressure_count": s.rspStats.TargetStdinBackpressureCount,
		"target_stdin_ready_count":        s.rspStats.TargetStdinReadyCount,
		"rsp_pending_to_target_bytes":     pendingToTarget,
		"rsp_pending_to_gdb_bytes":        s.rspStats.PendingToGDBBytes,
		"last_rsp_progress_at":            lastProgress,
		"rsp_trace_entries":               len(s.rspTrace.Entries),
		"rsp_trace_bytes":                 s.rspTrace.retainedBytes(),
		"rsp_trace_truncated":             s.rspTrace.truncated(),
		"rsp_trace_dropped_entries":       s.rspTrace.droppedEntries(),
		"rsp_trace_dropped_bytes":         s.rspTrace.droppedBytes(),
		"gdb_mi_bytes":                    s.hostMI.len(),
		"gdb_stderr_bytes": func() int {
			if s.gdb == nil {
				return 0
			}
			return len(s.gdb.Diagnostics())
		}(),
		"gdbserver_stderr_bytes": s.targetDiag.len(),
	}
}

func (s *Session) summary() map[string]any {
	s.mu.Lock()
	defer s.mu.Unlock()
	return map[string]any{"debug_token": s.Token, "label": s.Label, "mode": s.Mode, "remote_debug": s.RemoteDebug, "gdb_remote_timeout_seconds": s.mgr.cfg.GDBRemoteTimeoutSeconds, "mi_async": true, "interrupt_transport": "gdb-rsp", "interrupt_stop_requires_wait": true, "state": s.State, "pid": s.PID, "program": s.Program}
}

func (s *Session) DiagnosticsRead(req DiagnosticsRequest) (map[string]any, error) {
	source := req.Source
	switch source {
	case "rsp", "gdb_mi", "gdb_stderr", "gdbserver_stderr":
	default:
		return nil, errors.New("INVALID_ARGUMENT: source must be rsp, gdb_mi, gdb_stderr, or gdbserver_stderr")
	}
	maxBytes := req.MaxBytes
	if maxBytes <= 0 {
		maxBytes = 8192
	}
	if maxBytes > 64*1024 {
		maxBytes = 64 * 1024
	}
	maxEvents := req.MaxEvents
	if maxEvents <= 0 {
		maxEvents = 64
	}
	if maxEvents > 256 {
		maxEvents = 256
	}
	pos := 0
	if req.Cursor != "" {
		cur, err := parseDiagCursor(req.Cursor)
		if err != nil || cur.Source != source {
			return nil, errors.New("INVALID_ARGUMENT: bad diagnostics cursor")
		}
		pos = cur.Pos
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	if source == "rsp" {
		return s.readRSPDiagnosticsLocked(pos, maxBytes, maxEvents, req.Contains)
	}
	text := ""
	switch source {
	case "gdb_mi":
		text = s.hostMI.string()
	case "gdb_stderr":
		if s.gdb != nil {
			text = s.gdb.Diagnostics()
		}
	case "gdbserver_stderr":
		text = s.targetDiag.string()
	}
	return readTextDiagnostics(s.Token, source, text, pos, maxBytes, maxEvents, req.Contains)
}

func (s *Session) readRSPDiagnosticsLocked(pos, maxBytes, maxEvents int, contains string) (map[string]any, error) {
	entries := s.rspTrace.Entries
	if pos < 0 || pos > len(entries) {
		return nil, errors.New("CURSOR_EXPIRED")
	}
	items := []map[string]any{}
	used := 0
	next := pos
	for i := pos; i < len(entries); i++ {
		item := rspTraceItem(entries[i])
		if contains != "" && !rspTraceContains(entries[i], contains) {
			next = i + 1
			continue
		}
		itemBytes := len(entries[i].Hex) / 2
		if itemBytes == 0 {
			itemBytes = len(entries[i].Escaped)
		}
		if len(items) >= maxEvents || (used > 0 && used+itemBytes > maxBytes) {
			break
		}
		items = append(items, item)
		used += itemBytes
		next = i + 1
	}
	atEnd := true
	for i := next; i < len(entries); i++ {
		if contains == "" || rspTraceContains(entries[i], contains) {
			atEnd = false
			break
		}
	}
	return map[string]any{
		"debug_token":           s.Token,
		"source":                "rsp",
		"items":                 items,
		"next_cursor":           diagCursorFor("rsp", next),
		"at_end":                atEnd,
		"truncated":             s.rspTrace.truncated(),
		"trace_truncated":       s.rspTrace.truncated(),
		"trace_dropped_entries": s.rspTrace.droppedEntries(),
		"trace_dropped_bytes":   s.rspTrace.droppedBytes(),
		"trace_entries":         len(entries),
		"trace_bytes":           s.rspTrace.retainedBytes(),
	}, nil
}

func rspTraceItem(e rspTraceEntry) map[string]any {
	row := map[string]any{
		"direction": e.Direction,
		"seq":       e.Seq,
		"len":       e.Len,
	}
	if e.Dropped {
		row["dropped"] = true
	}
	if e.Hex != "" {
		row["hex"] = e.Hex
	}
	if e.Escaped != "" {
		row["escaped"] = e.Escaped
	}
	return row
}

func rspTraceContains(e rspTraceEntry, needle string) bool {
	if strings.Contains(e.Hex, needle) || strings.Contains(e.Escaped, needle) {
		return true
	}
	if e.Hex == "" {
		return false
	}
	b, err := hex.DecodeString(e.Hex)
	return err == nil && strings.Contains(string(b), needle)
}

func readTextDiagnostics(token, source, text string, pos, maxBytes, maxEvents int, contains string) (map[string]any, error) {
	if pos < 0 || pos > len(text) {
		return nil, errors.New("CURSOR_EXPIRED")
	}
	items := []map[string]any{}
	used := 0
	next := pos
	for next < len(text) && len(items) < maxEvents && used < maxBytes {
		start := next
		if contains != "" {
			idx := strings.Index(text[start:], contains)
			if idx < 0 {
				next = len(text)
				break
			}
			start += idx
			lineStart := strings.LastIndex(text[:start], "\n") + 1
			lineEndRel := strings.Index(text[start:], "\n")
			lineEnd := len(text)
			if lineEndRel >= 0 {
				lineEnd = start + lineEndRel + 1
			}
			start = lineStart
			next = lineEnd
		} else {
			lineEndRel := strings.Index(text[start:], "\n")
			next = len(text)
			if lineEndRel >= 0 {
				next = start + lineEndRel + 1
			}
		}
		if next <= start {
			next = start + 1
		}
		end := next
		if end-start > maxBytes-used {
			end = start + (maxBytes - used)
			next = end
		}
		if end <= start {
			break
		}
		chunk := text[start:end]
		items = append(items, map[string]any{"offset": start, "text": chunk})
		used += len(chunk)
	}
	atEnd := true
	if contains == "" {
		atEnd = next >= len(text)
	} else if next < len(text) && strings.Contains(text[next:], contains) {
		atEnd = false
	}
	return map[string]any{
		"debug_token": token,
		"source":      source,
		"items":       items,
		"next_cursor": diagCursorFor(source, next),
		"at_end":      atEnd,
		"truncated":   false,
		"bytes":       len(text),
	}, nil
}

func diagCursorFor(source string, pos int) string {
	b, err := json.Marshal(diagCursor{Source: source, Pos: pos})
	if err != nil {
		return ""
	}
	return "dd:" + base64.RawURLEncoding.EncodeToString(b)
}

func parseDiagCursor(s string) (diagCursor, error) {
	if !strings.HasPrefix(s, "dd:") {
		return diagCursor{}, errors.New("bad cursor")
	}
	b, err := base64.RawURLEncoding.DecodeString(strings.TrimPrefix(s, "dd:"))
	if err != nil {
		return diagCursor{}, err
	}
	var cur diagCursor
	if err := json.Unmarshal(b, &cur); err != nil {
		return diagCursor{}, err
	}
	if cur.Source == "" || cur.Pos < 0 {
		return diagCursor{}, errors.New("bad cursor")
	}
	return cur, nil
}

func (s *Session) Breakpoint(ctx context.Context, loc string, temporary bool, condition string) (map[string]any, error) {
	cmd := "-break-insert "
	if temporary {
		cmd += "-t "
	}
	if condition != "" {
		cmd += "-c " + miQuote(condition) + " "
	}
	rec, err := s.gdb.Command(ctx, cmd+miQuote(loc))
	if err != nil {
		return nil, err
	}
	v, _ := rec.Result("bkpt")
	out := map[string]any{"resolved": false}
	for _, kv := range v.Tuple {
		switch kv.Key {
		case "number":
			out["breakpoint_id"] = kv.Value.StringValue()
		case "addr":
			out["address"] = kv.Value.StringValue()
			if kv.Value.StringValue() != "<PENDING>" {
				out["resolved"] = true
			}
		case "func":
			out["function"] = kv.Value.StringValue()
		case "file", "fullname":
			out[kv.Key] = kv.Value.StringValue()
		case "line":
			out["line"] = kv.Value.StringValue()
		}
	}
	if s.mgr.journal != nil {
		s.mgr.journal("DEBUG_BREAKPOINT", map[string]any{"debug": s.Token, "location": loc, "id": out["breakpoint_id"]})
	}
	return out, nil
}

func (s *Session) BreakpointDelete(ctx context.Context, id string) (map[string]any, error) {
	if _, err := s.gdb.Command(ctx, "-break-delete "+miQuote(id)); err != nil {
		return nil, err
	}
	return map[string]any{"deleted": true, "breakpoint_id": id}, nil
}

func (s *Session) Continue(ctx context.Context) (map[string]any, error) {
	if _, err := s.gdb.Command(ctx, "-exec-continue"); err != nil {
		return nil, err
	}
	s.setRunningIfActive()
	if s.mgr.journal != nil {
		s.mgr.journal("DEBUG_RUNNING", map[string]any{"debug": s.Token})
	}
	return s.Status(), nil
}

func (s *Session) Interrupt(ctx context.Context) (map[string]any, error) {
	if _, err := s.gdb.Command(ctx, "-exec-interrupt --all"); err != nil {
		return nil, err
	}
	if s.mgr.journal != nil {
		s.mgr.journal("DEBUG_INTERRUPT", map[string]any{"debug": s.Token})
	}
	return s.Status(), nil
}

func (s *Session) Step(ctx context.Context, kind string) (map[string]any, error) {
	cmd := map[string]string{"step": "-exec-step", "next": "-exec-next", "finish": "-exec-finish", "stepi": "-exec-step-instruction", "nexti": "-exec-next-instruction"}[kind]
	if cmd == "" {
		return nil, errors.New("INVALID_ARGUMENT: kind must be step, next, finish, stepi, or nexti")
	}
	if _, err := s.gdb.Command(ctx, cmd); err != nil {
		return nil, err
	}
	s.setRunningIfActive()
	return s.Status(), nil
}

func (s *Session) Threads(ctx context.Context) (map[string]any, error) {
	rec, err := s.gdb.Command(ctx, "-thread-info")
	if err != nil {
		return nil, err
	}
	threads := []map[string]any{}
	if v, ok := rec.Result("threads"); ok {
		for _, tv := range v.List {
			row := map[string]any{}
			for _, kv := range tv.Tuple {
				row[kv.Key] = kv.Value.StringValue()
			}
			threads = append(threads, row)
			if len(threads) >= 256 {
				break
			}
		}
	}
	return map[string]any{"threads": threads}, nil
}

func (s *Session) SelectThread(ctx context.Context, id string) (map[string]any, error) {
	if _, err := s.gdb.Command(ctx, "-thread-select "+miQuote(id)); err != nil {
		return nil, err
	}
	s.ThreadID = id
	return map[string]any{"selected": true, "thread_id": id}, nil
}

func (s *Session) Backtrace(ctx context.Context, max int) (map[string]any, error) {
	if max <= 0 || max > 128 {
		max = 128
	}
	rec, err := s.gdb.Command(ctx, fmt.Sprintf("-stack-list-frames 0 %d", max-1))
	if err != nil {
		return nil, err
	}
	frames, err := framesFromRecord(rec)
	if err != nil {
		return nil, err
	}
	return map[string]any{"frames": frames}, nil
}

func (s *Session) Evaluate(ctx context.Context, expr string) (map[string]any, error) {
	rec, err := s.gdb.Command(ctx, "-data-evaluate-expression "+miQuote(expr))
	if err != nil {
		return nil, err
	}
	v, _ := rec.Result("value")
	return map[string]any{"value": v.StringValue()}, nil
}

func (s *Session) Registers(ctx context.Context) (map[string]any, error) {
	rec, err := s.gdb.Command(ctx, "-data-list-register-values x")
	if err != nil {
		return nil, err
	}
	return map[string]any{"raw": rec.Results}, nil
}

func (s *Session) MemoryRead(ctx context.Context, expr string, length int) (map[string]any, error) {
	if length <= 0 || length > MaxMemoryRead {
		return nil, errors.New("INVALID_ARGUMENT: length out of range")
	}
	rec, err := s.gdb.Command(ctx, fmt.Sprintf("-data-read-memory-bytes %s %d", miQuote(expr), length))
	if err != nil {
		return nil, err
	}
	v, _ := rec.Result("memory")
	var hexData string
	if len(v.List) > 0 {
		for _, kv := range v.List[0].Tuple {
			if kv.Key == "contents" {
				hexData = kv.Value.StringValue()
			}
		}
	}
	b, _ := hex.DecodeString(hexData)
	return map[string]any{"base64": base64.StdEncoding.EncodeToString(b), "bytes": len(b)}, nil
}

func (s *Session) Wait(ctx context.Context, timeout time.Duration) map[string]any {
	if timeout <= 0 || timeout > 20*time.Second {
		timeout = 3 * time.Second
	}
	s.mu.Lock()
	ch := s.wait
	initial := s.State + s.StopReason
	s.mu.Unlock()
	timer := time.NewTimer(timeout)
	defer timer.Stop()
	changed := false
	select {
	case <-ch:
		changed = true
	case <-timer.C:
	case <-ctx.Done():
	}
	st := s.Status()
	s.mu.Lock()
	changed = changed || initial != s.State+s.StopReason
	s.mu.Unlock()
	st["changed"] = changed
	return st
}

func (s *Session) Detach(ctx context.Context) (map[string]any, error) {
	if s.gdb != nil {
		_, _ = s.gdb.Command(ctx, "-target-detach")
		_, _ = s.gdb.Command(ctx, "-gdb-exit")
	}
	s.setInferiorCleanup("detached")
	s.setState("DETACHED", "")
	s.cleanup(ctx, false, true)
	if s.mgr.journal != nil {
		s.mgr.journal("DEBUG_DETACH", map[string]any{"debug": s.Token})
	}
	return map[string]any{"detached": true, "debug_token": s.Token}, nil
}

func (s *Session) Stop(ctx context.Context) error {
	if s.isTerminal() {
		s.cleanup(ctx, true, true)
		if s.mgr.journal != nil {
			s.mgr.journal("DEBUG_STOP", map[string]any{"debug": s.Token})
		}
		return nil
	}
	if s.gdb != nil {
		_ = s.gdb.Close()
	}
	s.setState("ERROR", "stopped")
	s.cleanup(ctx, true, true)
	if s.mgr.journal != nil {
		s.mgr.journal("DEBUG_STOP", map[string]any{"debug": s.Token})
	}
	return nil
}

func (s *Session) acceptLoop() {
	s.mu.Lock()
	s.acceptAttempted = true
	s.mu.Unlock()
	conn, err := s.ln.Accept()
	if err != nil {
		select {
		case <-s.done:
			return
		default:
		}
		s.mu.Lock()
		s.acceptError = err.Error()
		s.mu.Unlock()
		s.fail("ERROR", fmt.Errorf("RSP_ACCEPT_FAILED: %w", err))
		return
	}
	s.mu.Lock()
	s.conn = conn
	s.acceptSucceeded = true
	s.acceptedAt = time.Now()
	s.mu.Unlock()
	closeOnce(s.connReady)
	buf := make([]byte, MaxRSPChunk)
	for {
		n, err := conn.Read(buf)
		if n > 0 {
			s.addGDBRx(n)
			if !s.enqueueRSP(buf[:n]) {
				return
			}
		}
		if err != nil {
			select {
			case <-s.done:
				return
			default:
			}
			localClosed := isLocalSocketClosed(err)
			if localClosed {
				s.mu.Lock()
				if s.rspLocalClose == "" {
					s.rspLocalClose = "RSP_LOCAL_CLOSED: gdb unix socket closed"
				}
				s.mu.Unlock()
			}
			if s.stateIs("CONNECTING") {
				s.mu.Lock()
				if localClosed {
					s.rspLocalClose = "RSP_LOCAL_CLOSED: gdb unix socket closed"
				} else {
					s.rspLocalClose = fmt.Sprintf("RSP_LOCAL_READ_FAILED: %v", err)
				}
				s.mu.Unlock()
				return
			}
			if localClosed {
				s.fail("LOST", errors.New("RSP_LOCAL_CLOSED: gdb unix socket closed"))
			} else {
				s.fail("LOST", fmt.Errorf("RSP_LOCAL_READ_FAILED: %w", err))
			}
			return
		}
	}
}

func (s *Session) enqueueRSP(data []byte) bool {
	for len(data) > 0 {
		s.rspMu.Lock()
		space := MaxRSPPendingBytes - len(s.rspPending)
		if space > 0 {
			n := len(data)
			if n > space {
				n = space
			}
			s.rspPending = append(s.rspPending, data[:n]...)
			data = data[n:]
			s.rspMu.Unlock()
			s.signalRSPData()
			continue
		}
		s.rspMu.Unlock()
		select {
		case <-s.rspSpace:
		case <-s.done:
			return false
		}
	}
	return true
}

func (s *Session) writeTargetStdin() {
	for {
		chunk, ok := s.nextRSPChunk()
		if !ok {
			return
		}
		readyGen := s.stdinReadyGeneration()
		cctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
		_, err := s.mgr.hc.Request(cctx, hc2.TypeExecStdinData, mustStdinPayload(s.ExecRef, chunk))
		cancel()
		if err == nil {
			s.addTargetTx(len(chunk))
			s.traceG2T(chunk)
			s.discardAcceptedRSP(len(chunk))
			continue
		}
		if isStdinBackpressure(err) {
			s.addTargetBackpressure()
			if s.waitStdinReady(readyGen) {
				continue
			}
			return
		}
		if isClosedTargetStdin(err) {
			s.fail("LOST", fmt.Errorf("RSP_TARGET_STDIN_CLOSED: ref=%d bytes=%d: %w", s.ExecRef, len(chunk), err))
		} else {
			s.fail("ERROR", fmt.Errorf("RSP_TARGET_WRITE_FAILED: ref=%d bytes=%d: %w", s.ExecRef, len(chunk), err))
		}
		s.mu.Lock()
		conn := s.conn
		s.mu.Unlock()
		if conn != nil {
			_ = conn.Close()
		}
		return
	}
}

func (s *Session) nextRSPChunk() ([]byte, bool) {
	for {
		s.rspMu.Lock()
		if len(s.rspPending) > 0 {
			n := len(s.rspPending)
			if n > MaxRSPChunk {
				n = MaxRSPChunk
			}
			chunk := append([]byte(nil), s.rspPending[:n]...)
			s.rspMu.Unlock()
			return chunk, true
		}
		s.rspMu.Unlock()
		select {
		case <-s.rspData:
		case <-s.done:
			return nil, false
		}
	}
}

func (s *Session) discardAcceptedRSP(n int) {
	s.rspMu.Lock()
	if n > len(s.rspPending) {
		n = len(s.rspPending)
	}
	copy(s.rspPending, s.rspPending[n:])
	s.rspPending = s.rspPending[:len(s.rspPending)-n]
	remaining := len(s.rspPending)
	s.rspMu.Unlock()
	s.signalRSPSpace()
	if remaining > 0 {
		s.signalRSPData()
	}
}

func (s *Session) signalRSPData() {
	s.signal(s.rspData)
}

func (s *Session) signalRSPSpace() {
	s.signal(s.rspSpace)
}

func (s *Session) notifyStdinReady() {
	s.rspMu.Lock()
	s.readyGen++
	s.rspMu.Unlock()
	s.mu.Lock()
	s.rspStats.TargetStdinReadyCount++
	s.mu.Unlock()
	s.signal(s.stdinReady)
}

func (s *Session) stdinReadyGeneration() uint64 {
	s.rspMu.Lock()
	defer s.rspMu.Unlock()
	return s.readyGen
}

func (s *Session) waitStdinReady(gen uint64) bool {
	for {
		if s.isTerminal() {
			return false
		}
		s.rspMu.Lock()
		changed := s.readyGen != gen
		s.rspMu.Unlock()
		if changed {
			return true
		}
		select {
		case <-s.stdinReady:
		case <-s.done:
			return false
		}
	}
}

func (s *Session) isTerminal() bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.State == "ERROR" || s.State == "LOST" || s.State == "DETACHED" || s.State == "EXITED"
}

func (s *Session) isActive() bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	return !s.cleaned && s.State != "ERROR" && s.State != "LOST" && s.State != "DETACHED" && s.State != "EXITED"
}

func (s *Session) stateIs(state string) bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.State == state
}

func (s *Session) signal(ch chan struct{}) {
	if ch == nil {
		return
	}
	select {
	case ch <- struct{}{}:
	default:
	}
}

func isStdinBackpressure(err error) bool {
	var he *hc2.Error
	return errors.As(err, &he) && he.Code == hc2.ErrBusy && strings.Contains(he.Message, "stdin backpressure")
}

func isClosedTargetStdin(err error) bool {
	var he *hc2.Error
	if !errors.As(err, &he) {
		return errors.Is(err, io.ErrClosedPipe)
	}
	switch he.Code {
	case hc2.ErrNotFound, hc2.ErrStaleTarget, hc2.ErrTargetLost, hc2.ErrNotConnected:
		return true
	default:
		return false
	}
}

func isLocalSocketClosed(err error) bool {
	return err == io.EOF || strings.Contains(err.Error(), "use of closed network connection")
}

func (s *Session) markRSPProgressLocked() {
	s.rspStats.LastProgress = time.Now()
}

func (s *Session) addGDBRx(n int) {
	s.mu.Lock()
	s.rspStats.GDBRxBytes += uint64(n)
	s.markRSPProgressLocked()
	s.mu.Unlock()
}

func (s *Session) addTargetTx(n int) {
	s.mu.Lock()
	s.rspStats.TargetTxBytes += uint64(n)
	s.markRSPProgressLocked()
	s.mu.Unlock()
}

func (s *Session) traceG2T(data []byte) {
	s.mu.Lock()
	s.rspTrace.record("G2T", data)
	s.mu.Unlock()
}

func (s *Session) traceT2G(data []byte) {
	s.mu.Lock()
	s.rspTrace.record("T2G", data)
	s.mu.Unlock()
}

func (t *rspTrace) record(direction string, data []byte) {
	entry := rspTraceEntry{Direction: direction, Len: len(data)}
	var used *int
	switch direction {
	case "G2T":
		t.G2TSeq++
		entry.Seq = t.G2TSeq
		used = &t.G2TBytes
	case "T2G":
		t.T2GSeq++
		entry.Seq = t.T2GSeq
		used = &t.T2GBytes
	default:
		return
	}
	if *used >= maxRSPTraceBytesPerDirection {
		entry.Dropped = true
		t.Entries = append(t.Entries, entry)
		return
	}
	n := len(data)
	if n > maxRSPTraceBytesPerDirection-*used {
		n = maxRSPTraceBytesPerDirection - *used
		entry.Dropped = true
	}
	*used += n
	sample := data[:n]
	entry.Hex = hex.EncodeToString(sample)
	entry.Escaped = strconv.QuoteToASCII(string(sample))
	t.Entries = append(t.Entries, entry)
}

func (t rspTrace) snapshot() []map[string]any {
	out := make([]map[string]any, 0, len(t.Entries))
	for _, e := range t.Entries {
		row := map[string]any{
			"direction": e.Direction,
			"seq":       e.Seq,
			"len":       e.Len,
		}
		if e.Dropped {
			row["dropped"] = true
		}
		if e.Hex != "" {
			row["hex"] = e.Hex
		}
		if e.Escaped != "" {
			row["escaped"] = e.Escaped
		}
		out = append(out, row)
	}
	return out
}

func (t rspTrace) retainedBytes() int {
	n := 0
	for _, e := range t.Entries {
		n += len(e.Hex) / 2
	}
	return n
}

func (t rspTrace) truncated() bool {
	for _, e := range t.Entries {
		if e.Dropped {
			return true
		}
	}
	return false
}

func (t rspTrace) droppedEntries() int {
	n := 0
	for _, e := range t.Entries {
		if e.Dropped {
			n++
		}
	}
	return n
}

func (t rspTrace) droppedBytes() int {
	n := 0
	for _, e := range t.Entries {
		retained := len(e.Hex) / 2
		if e.Len > retained {
			n += e.Len - retained
		}
	}
	return n
}

func (s *Session) addTargetRx(n int) {
	s.mu.Lock()
	s.rspStats.TargetRxBytes += uint64(n)
	s.rspStats.PendingToGDBBytes += uint64(n)
	s.markRSPProgressLocked()
	s.mu.Unlock()
}

func (s *Session) addGDBTx(n int) {
	s.mu.Lock()
	if uint64(n) > s.rspStats.PendingToGDBBytes {
		s.rspStats.PendingToGDBBytes = 0
	} else {
		s.rspStats.PendingToGDBBytes -= uint64(n)
	}
	s.rspStats.GDBTxBytes += uint64(n)
	s.markRSPProgressLocked()
	s.mu.Unlock()
}

func (s *Session) dropPendingToGDB(n int) {
	s.mu.Lock()
	if uint64(n) > s.rspStats.PendingToGDBBytes {
		s.rspStats.PendingToGDBBytes = 0
	} else {
		s.rspStats.PendingToGDBBytes -= uint64(n)
	}
	s.mu.Unlock()
}

func (s *Session) addTargetBackpressure() {
	s.mu.Lock()
	s.rspStats.TargetStdinBackpressureCount++
	s.mu.Unlock()
}

func (s *Session) writeToGDB() {
	for {
		select {
		case data := <-s.toGDB:
			select {
			case <-s.connReady:
			case <-s.done:
				return
			}
			s.mu.Lock()
			conn := s.conn
			s.mu.Unlock()
			if conn == nil {
				s.fail("ERROR", errors.New("RSP_GDB_WRITE_FAILED: no accepted gdb socket"))
				return
			}
			if err := s.writeFullToGDB(conn, data); err != nil {
				s.fail("ERROR", err)
				return
			}
		case <-s.done:
			return
		}
	}
}

func (s *Session) writeFullToGDB(conn net.Conn, data []byte) error {
	off := 0
	for off < len(data) {
		n, err := conn.Write(data[off:])
		if n > 0 {
			written := data[off : off+n]
			s.addGDBTx(n)
			s.traceT2G(written)
			off += n
		}
		if err != nil {
			return fmt.Errorf("RSP_GDB_WRITE_FAILED: bytes=%d/%d: %w", off, len(data), err)
		}
		if n == 0 {
			return fmt.Errorf("RSP_GDB_WRITE_FAILED: bytes=%d/%d: zero-byte write", off, len(data))
		}
	}
	return nil
}

func (s *Session) gdbEvents() {
	for ev := range s.gdb.Events() {
		rec := ev.Record
		switch rec.Kind {
		case gdbmi.ExecAsync:
			switch rec.Class {
			case "running":
				s.setState("RUNNING", "")
			case "stopped":
				reason := ""
				if v, ok := rec.Result("reason"); ok {
					reason = v.StringValue()
				}
				if v, ok := rec.Result("thread-id"); ok {
					s.mu.Lock()
					s.ThreadID = v.StringValue()
					s.mu.Unlock()
				}
				if v, ok := rec.Result("frame"); ok {
					s.mu.Lock()
					s.Frame, _ = frameMapFromValue(v, false)
					s.mu.Unlock()
				}
				s.setState("STOPPED", reason)
				if s.mgr.journal != nil {
					s.mgr.journal("DEBUG_STOPPED", map[string]any{"debug": s.Token, "reason": reason})
				}
			}
		case gdbmi.ConsoleStream, gdbmi.TargetStream, gdbmi.LogStream:
			s.mu.Lock()
			s.hostMI.add([]byte(rec.Stream))
			s.mu.Unlock()
		}
	}
	if err := s.gdb.Err(); err != nil {
		s.fail("LOST", err)
		return
	}
	s.fail("LOST", errors.New("GDB_EXITED: event stream closed"))
}

func (s *Session) setState(state, reason string) {
	s.mu.Lock()
	if (s.State == "ERROR" || s.State == "LOST" || s.State == "DETACHED") &&
		state != "ERROR" && state != "LOST" && state != "DETACHED" {
		s.mu.Unlock()
		return
	}
	s.State = state
	if reason != "" {
		s.StopReason = reason
	}
	old := s.wait
	s.wait = make(chan struct{})
	s.mu.Unlock()
	close(old)
}

func (s *Session) setRunningIfActive() {
	s.mu.Lock()
	switch s.State {
	case "STOPPED", "EXITED", "ERROR", "LOST", "DETACHED":
		s.mu.Unlock()
		return
	}
	s.State = "RUNNING"
	old := s.wait
	s.wait = make(chan struct{})
	s.mu.Unlock()
	close(old)
}

func (s *Session) fail(state string, err error) {
	s.mu.Lock()
	if s.State == "DETACHED" || s.State == "ERROR" || s.State == "LOST" {
		s.mu.Unlock()
		return
	}
	cancel := s.connectCancel
	s.State = state
	if err != nil {
		s.LastError = err.Error()
	}
	old := s.wait
	s.wait = make(chan struct{})
	s.mu.Unlock()
	if cancel != nil {
		cancel()
	}
	s.signal(s.stdinReady)
	s.signalRSPData()
	s.signalRSPSpace()
	close(old)
	if s.mgr.journal != nil {
		event := "DEBUG_ERROR"
		if state == "LOST" {
			event = "DEBUG_LOST"
		}
		s.mgr.journal(event, map[string]any{"debug": s.Token, "error": s.LastError})
	}
	go s.cleanup(context.Background(), true, false)
}

func (s *Session) cleanup(ctx context.Context, force bool, removeSession bool) {
	s.cleanupMu.Lock()
	alreadyCleaned := s.cleaned
	if !alreadyCleaned {
		s.cleaned = true
		s.tombstoneAt = time.Now()
	}
	s.cleanupMu.Unlock()
	s.mu.Lock()
	cancel := s.connectCancel
	s.connectCancel = nil
	ln := s.ln
	conn := s.conn
	gdb := s.gdb
	ref := s.ExecRef
	s.mu.Unlock()
	if !alreadyCleaned {
		if force && s.Mode == "attach" {
			s.cleanupAttachInferior(ctx, gdb, ref)
		}
		closeOnce(s.done)
	}
	if !alreadyCleaned {
		if cancel != nil {
			cancel()
		}
		if ln != nil {
			_ = ln.Close()
		}
		if conn != nil {
			_ = conn.Close()
		}
		if gdb != nil && force {
			_ = gdb.Close()
		}
		if ref != 0 && force {
			if s.waitExecDone(ctx, 100*time.Millisecond) {
				s.releaseExec(ctx, ref)
			} else {
				s.forceTerminateExec(ctx, ref)
			}
		} else if ref != 0 {
			s.releaseExec(ctx, ref)
		}
		_ = os.Remove(s.socketPath)
		s.mgr.mu.Lock()
		delete(s.mgr.byRef, ref)
		s.mgr.pruneTombstonesLocked(16)
		s.mgr.mu.Unlock()
	}
	if removeSession {
		_ = os.Remove(s.socketPath)
	}
	if removeSession {
		s.mgr.mu.Lock()
		delete(s.mgr.byRef, ref)
		delete(s.mgr.sessions, s.Token)
		s.mgr.mu.Unlock()
	}
}

func (s *Session) cleanupAttachInferior(ctx context.Context, gdb *gdbmi.Process, ref uint64) {
	if ref == 0 {
		return
	}
	if gdb != nil {
		cctx, cancel := context.WithTimeout(ctx, 2*time.Second)
		_, err := gdb.Command(cctx, "-target-detach")
		cancel()
		if err == nil {
			s.setInferiorCleanup("detached")
			if s.waitExecDone(ctx, 2*time.Second) {
				return
			}
		}
	}
	if s.requestExecStdinClose(ctx, ref) {
		s.setInferiorCleanup("gdbserver-stdin-eof-attempted")
		if s.waitExecDone(ctx, 2*time.Second) {
			s.setInferiorCleanup("graceful-gdbserver-exit")
			return
		}
	}
	s.setInferiorCleanup("forced-unconfirmed")
}

func (s *Session) requestExecStdinClose(ctx context.Context, ref uint64) bool {
	cctx, cancel := context.WithTimeout(ctx, 2*time.Second)
	_, err := s.mgr.hc.Request(cctx, hc2.TypeExecStdinClose, hc2.U64Payload(ref))
	cancel()
	return err == nil
}

func (s *Session) waitExecDone(ctx context.Context, d time.Duration) bool {
	if s.execDone == nil {
		return false
	}
	timer := time.NewTimer(d)
	defer timer.Stop()
	select {
	case <-s.execDone:
		return true
	case <-timer.C:
		return false
	case <-ctx.Done():
		return false
	}
}

func (s *Session) forceTerminateExec(ctx context.Context, ref uint64) {
	cctx, cancel := context.WithTimeout(ctx, 2*time.Second)
	_, _ = s.mgr.hc.Request(cctx, hc2.TypeExecSignal, hc2.SignalPayload(ref, hc2.SigTERM))
	cancel()
	if !s.waitExecDone(ctx, 500*time.Millisecond) {
		cctx, cancel = context.WithTimeout(ctx, 2*time.Second)
		_, _ = s.mgr.hc.Request(cctx, hc2.TypeExecSignal, hc2.SignalPayload(ref, hc2.SigKILL))
		cancel()
		_ = s.waitExecDone(ctx, 500*time.Millisecond)
	}
	s.releaseExec(ctx, ref)
}

func (s *Session) releaseExec(ctx context.Context, ref uint64) {
	cctx, cancel := context.WithTimeout(ctx, 3*time.Second)
	_, _ = s.mgr.hc.Request(cctx, hc2.TypeExecRelease, hc2.U64Payload(ref))
	cancel()
}

func (s *Session) setInferiorCleanup(v string) {
	if v == "" {
		return
	}
	s.mu.Lock()
	if s.InferiorCleanup == "" ||
		v == "forced-unconfirmed" ||
		(v == "detached" && s.InferiorCleanup != "forced-unconfirmed") ||
		(v == "graceful-gdbserver-exit" && s.InferiorCleanup == "gdbserver-stdin-eof-attempted") {
		s.InferiorCleanup = v
	}
	s.mu.Unlock()
}

func (m *Manager) pruneTombstonesLocked(limit int) {
	if limit <= 0 {
		return
	}
	count := 0
	for _, s := range m.sessions {
		if !s.isActive() {
			count++
		}
	}
	for count > limit {
		var oldest *Session
		for _, s := range m.sessions {
			if s.isActive() {
				continue
			}
			if oldest == nil || s.tombstoneAt.Before(oldest.tombstoneAt) {
				oldest = s
			}
		}
		if oldest == nil {
			return
		}
		delete(m.sessions, oldest.Token)
		count--
	}
}

func closeOnce(ch chan struct{}) {
	defer func() { _ = recover() }()
	close(ch)
}

func mustStdinPayload(ref uint64, data []byte) []byte {
	p, err := hc2.StdinPayload(ref, data)
	if err != nil {
		panic(err)
	}
	return p
}

func miQuote(s string) string {
	return strconv.Quote(s)
}

func framesFromRecord(rec gdbmi.Record) ([]map[string]any, error) {
	v, ok := rec.Result("stack")
	if !ok {
		return nil, errors.New("GDB_MI_DATA_ERROR: missing stack")
	}
	if v.Kind != gdbmi.List {
		return nil, errors.New("GDB_MI_DATA_ERROR: stack is not a list")
	}
	out := []map[string]any{}
	for _, item := range v.List {
		if item.Kind != gdbmi.Tuple {
			return nil, errors.New("GDB_MI_DATA_ERROR: stack entry is not a tuple")
		}
		if len(item.Tuple) > 0 && allTupleKeys(item.Tuple, "frame") {
			for _, kv := range item.Tuple {
				frame, err := frameMapFromValue(kv.Value, true)
				if err != nil {
					return nil, err
				}
				out = append(out, frame)
			}
			continue
		}
		frame, err := frameMapFromTuple(item.Tuple, true)
		if err != nil {
			return nil, err
		}
		out = append(out, frame)
	}
	return out, nil
}

func allTupleKeys(tuple []gdbmi.ResultKV, key string) bool {
	for _, kv := range tuple {
		if kv.Key != key {
			return false
		}
	}
	return len(tuple) > 0
}

func frameMapFromValue(v gdbmi.Value, required bool) (map[string]any, error) {
	if v.Kind != gdbmi.Tuple {
		return nil, errors.New("GDB_MI_DATA_ERROR: frame is not a tuple")
	}
	return frameMapFromTuple(v.Tuple, required)
}

func frameMapFromTuple(tuple []gdbmi.ResultKV, required bool) (map[string]any, error) {
	out := map[string]any{}
	for _, kv := range tuple {
		switch kv.Key {
		case "level", "addr", "func", "file", "fullname", "line", "from", "arch", "addr_flags":
			key := kv.Key
			if key == "addr" {
				key = "address"
			}
			if key == "func" {
				key = "function"
			}
			out[key] = kv.Value.StringValue()
		}
	}
	if required {
		level, hasLevel := out["level"].(string)
		address, hasAddress := out["address"].(string)
		if !hasLevel || level == "" || !hasAddress || address == "" {
			return nil, errors.New("GDB_MI_DATA_ERROR: stack frame missing level or addr")
		}
	}
	return out, nil
}

func randHex(v uint64) string {
	var b [8]byte
	binary.BigEndian.PutUint64(b[:], uint64(time.Now().UnixNano())^v)
	return hex.EncodeToString(b[:4])
}

func execStateName(st byte) string {
	switch st {
	case 1:
		return "STARTING"
	case 2:
		return "RUNNING"
	case 3:
		return "EXITED_DRAINING"
	case 4:
		return "DONE"
	case 5:
		return "START_FAILED"
	case 6:
		return "TERMINATING"
	default:
		return fmt.Sprintf("STATE_%d", st)
	}
}
