package mcpapp

import (
	"crypto/rand"
	"encoding/base64"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"
	"unicode/utf8"

	"lmux/internal/hc2"
)

const (
	DefaultMaxSessions               = 5
	DefaultSessionBytes              = 10 * 1024 * 1024
	DefaultGrace                     = 10 * time.Minute
	DefaultJournalSegment            = 512 * 1024
	DefaultJournalTotal              = DefaultJournalSegment * 2
	DefaultMaxSessionCommands        = 3
	DefaultMaxConcurrentCommand      = 4
	transcriptRecordHeader           = 17
	eventStdout                 byte = 1
	eventStderr                 byte = 2
	eventState                  byte = 3
	eventLost                   byte = 4
)

type Manager struct {
	RuntimeDir      string
	MaxSessions     int
	SessionBytes    int64
	Grace           time.Duration
	JournalSegment  int64
	SessionSequence uint64

	mu      sync.Mutex
	lock    *os.File
	session *Session
}

type Session struct {
	ID       string
	Dir      string
	Created  time.Time
	Attached bool

	MaxBytes       int64
	JournalSegment int64
	commands       map[string]*Command
	nextToken      uint64
	nextEvent      uint64
	floorEvent     uint64
	storageBytes   int64
	journalBytes   int64
	journal        *os.File
	mu             sync.Mutex
}

type Command struct {
	Token        string
	ExecRef      uint64
	TargetID     uint64
	Kind         string
	Argv         []string
	CWD          string
	State        string
	OutputDone   bool
	ExitCode     uint32
	Signal       uint32
	StartErrno   uint16
	LostReason   string
	Released     bool
	Created      time.Time
	LastVerified time.Time
	Events       []Event
	file         *os.File
	path         string
	bytes        int64
}

type CommandSnapshot struct {
	Token        string
	ExecRef      uint64
	TargetID     uint64
	Kind         string
	State        string
	LostReason   string
	Released     bool
	Created      time.Time
	LastVerified time.Time
}

type Event struct {
	Seq    uint64
	Kind   byte
	Stream string
	Data   []byte
	State  string
	Time   time.Time
}

func NewManager(runtime string) *Manager {
	return &Manager{
		RuntimeDir:     runtime,
		MaxSessions:    DefaultMaxSessions,
		SessionBytes:   DefaultSessionBytes,
		Grace:          DefaultGrace,
		JournalSegment: DefaultJournalSegment,
	}
}

func DefaultRuntimeDir() (string, error) {
	xdg := os.Getenv("XDG_RUNTIME_DIR")
	if xdg == "" {
		return "", errors.New("XDG_RUNTIME_DIR is not set; pass --runtime-dir")
	}
	if !filepath.IsAbs(xdg) {
		return "", fmt.Errorf("XDG_RUNTIME_DIR must be absolute: %s", xdg)
	}
	return filepath.Join(xdg, "lmux", "mcp"), nil
}

func (m *Manager) Start() (*Session, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	if m.RuntimeDir == "" {
		runtime, err := DefaultRuntimeDir()
		if err != nil {
			return nil, err
		}
		m.RuntimeDir = runtime
	}
	if err := os.MkdirAll(filepath.Join(m.RuntimeDir, "sessions"), 0700); err != nil {
		return nil, err
	}
	lock, err := os.OpenFile(filepath.Join(m.RuntimeDir, "owner.lock"), os.O_CREATE|os.O_RDWR, 0600)
	if err != nil {
		return nil, err
	}
	if err := tryLock(lock); err != nil {
		lock.Close()
		return nil, fmt.Errorf("lmux-mcp already running: %w", err)
	}
	m.lock = lock
	if err := m.cleanupStaleLocked(); err != nil {
		return nil, err
	}
	return m.createSessionLocked()
}

func (m *Manager) Close() {
	m.mu.Lock()
	defer m.mu.Unlock()
	if m.session != nil {
		m.session.Close()
	}
	if m.lock != nil {
		m.lock.Close()
		m.lock = nil
	}
}

func (m *Manager) cleanupStaleLocked() error {
	sessions := filepath.Join(m.RuntimeDir, "sessions")
	entries, err := os.ReadDir(sessions)
	if err != nil {
		return err
	}
	sort.Slice(entries, func(i, j int) bool { return entries[i].Name() < entries[j].Name() })
	for len(entries) >= m.MaxSessions && len(entries) > 0 {
		if err := os.RemoveAll(filepath.Join(sessions, entries[0].Name())); err != nil {
			return err
		}
		entries = entries[1:]
	}
	return nil
}

func (m *Manager) createSessionLocked() (*Session, error) {
	id := "s-" + randomHex(8)
	dir := filepath.Join(m.RuntimeDir, "sessions", "session-"+id)
	if err := os.MkdirAll(dir, 0700); err != nil {
		return nil, err
	}
	s := &Session{
		ID:             id,
		Dir:            dir,
		Created:        time.Now(),
		Attached:       true,
		MaxBytes:       m.SessionBytes,
		JournalSegment: m.JournalSegment,
		commands:       make(map[string]*Command),
		floorEvent:     1,
	}
	if err := s.openJournal(); err != nil {
		return nil, err
	}
	s.Journal("SESSION_OPEN", nil)
	tmp := filepath.Join(m.RuntimeDir, "current.tmp")
	_ = os.Remove(tmp)
	if err := os.Symlink(filepath.Join("sessions", "session-"+id), tmp); err == nil {
		_ = os.Rename(tmp, filepath.Join(m.RuntimeDir, "current"))
	}
	m.session = s
	return s, nil
}

func (s *Session) Close() {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.journal != nil {
		_ = s.journal.Close()
		s.journal = nil
	}
	for _, c := range s.commands {
		if c.file != nil {
			_ = c.file.Close()
			c.file = nil
		}
	}
}

func NormalizeCommandKind(kind string) (string, error) {
	if kind == "" {
		return "oneshot", nil
	}
	switch kind {
	case "oneshot", "session":
		return kind, nil
	default:
		return "", errors.New("INVALID_ARGUMENT: command kind must be oneshot or session")
	}
}

func commandActive(c *Command) bool {
	if c == nil || c.Released {
		return false
	}
	switch c.State {
	case "DONE", "FAILED", "START_FAILED", "LOST":
		return false
	default:
		return true
	}
}

func (s *Session) ActiveCommandCounts() (sessionCommands, totalCommands int) {
	s.mu.Lock()
	defer s.mu.Unlock()
	for _, c := range s.commands {
		if !commandActive(c) {
			continue
		}
		totalCommands++
		if c.Kind == "session" {
			sessionCommands++
		}
	}
	return sessionCommands, totalCommands
}

func (s *Session) NewCommand(argv []string, cwd string, execRef, targetID uint64, kind string) (*Command, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	kind, err := NormalizeCommandKind(kind)
	if err != nil {
		return nil, err
	}
	s.nextToken++
	token := fmt.Sprintf("c-%d-%s", s.nextToken, randomHex(4))
	path := filepath.Join(s.Dir, "command-"+safeName(token)+".transcript")
	f, err := os.OpenFile(path, os.O_CREATE|os.O_RDWR|os.O_TRUNC, 0600)
	if err != nil {
		return nil, err
	}
	c := &Command{Token: token, ExecRef: execRef, TargetID: targetID, Kind: kind, Argv: append([]string(nil), argv...), CWD: cwd, State: "STARTING", Created: time.Now(), file: f, path: path}
	s.commands[token] = c
	s.JournalLocked("COMMAND_START", map[string]any{"cmd": token, "kind": kind, "argv": argv, "cwd": cwd})
	return c, nil
}

func (s *Session) Command(token string) (*Command, bool) {
	s.mu.Lock()
	defer s.mu.Unlock()
	c, ok := s.commands[token]
	return c, ok
}

func (s *Session) ByExecRef(ref uint64) *Command {
	s.mu.Lock()
	defer s.mu.Unlock()
	for _, c := range s.commands {
		if c.ExecRef == ref && !c.Released {
			return c
		}
	}
	return nil
}

func commandTerminal(c *Command) bool {
	if c == nil {
		return true
	}
	switch c.State {
	case "DONE", "FAILED", "START_FAILED", "LOST":
		return true
	default:
		return false
	}
}

func (s *Session) ListCommands(includeTerminal bool, limit int) ([]CommandSnapshot, int) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if limit <= 0 {
		limit = 128
	}
	items := make([]CommandSnapshot, 0, len(s.commands))
	for _, c := range s.commands {
		if c.Released || (!includeTerminal && commandTerminal(c)) {
			continue
		}
		items = append(items, CommandSnapshot{
			Token: c.Token, ExecRef: c.ExecRef, TargetID: c.TargetID,
			Kind: c.Kind, State: c.State, LostReason: c.LostReason,
			Released: c.Released, Created: c.Created,
			LastVerified: c.LastVerified,
		})
	}
	sort.Slice(items, func(i, j int) bool {
		if items[i].Created.Equal(items[j].Created) {
			return items[i].Token < items[j].Token
		}
		return items[i].Created.Before(items[j].Created)
	})
	total := len(items)
	if len(items) > limit {
		items = items[len(items)-limit:]
	}
	return items, total
}

func (s *Session) AppendOutput(token, stream string, data []byte) error {
	kind := eventStdout
	if stream == "stderr" {
		kind = eventStderr
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	c := s.commands[token]
	if c == nil || c.Released {
		return errors.New("command not found")
	}
	return s.appendEventLocked(c, Event{Kind: kind, Stream: stream, Data: append([]byte(nil), data...), Time: time.Now()})
}

func (s *Session) UpdateState(token string, st hc2.ExecState) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	c := s.commands[token]
	if c == nil || c.Released {
		return errors.New("command not found")
	}
	c.State = stateName(st.State)
	c.LastVerified = time.Now()
	c.OutputDone = c.State == "DONE"
	c.ExitCode = st.ExitCode
	c.Signal = st.TermSignal
	c.StartErrno = st.Errno
	body, _ := json.Marshal(map[string]any{"state": c.State, "exit_code": c.ExitCode, "signal": c.Signal, "errno": c.StartErrno})
	if err := s.appendEventLocked(c, Event{Kind: eventState, State: c.State, Data: body, Time: time.Now()}); err != nil {
		return err
	}
	if c.State == "DONE" || c.State == "EXITED" || c.State == "SIGNALED" || c.State == "START_FAILED" {
		s.JournalLocked("COMMAND_EXIT", map[string]any{"cmd": token, "state": c.State, "exit_code": c.ExitCode, "signal": c.Signal})
	}
	return nil
}

func (s *Session) MarkLostByRef(ref uint64, reason uint32) {
	s.mu.Lock()
	defer s.mu.Unlock()
	for _, c := range s.commands {
		if c.ExecRef == ref && !c.Released {
			c.State = "LOST"
			c.LastVerified = time.Now()
			c.LostReason = fmt.Sprintf("%d", reason)
			_ = s.appendEventLocked(c, Event{Kind: eventLost, State: "LOST", Data: []byte(c.LostReason), Time: time.Now()})
			s.JournalLocked("COMMAND_LOST", map[string]any{"cmd": c.Token, "reason": reason})
		}
	}
}

func (s *Session) Release(token string) {
	s.mu.Lock()
	defer s.mu.Unlock()
	c := s.commands[token]
	if c == nil {
		return
	}
	if c.file != nil {
		_ = c.file.Close()
	}
	_ = os.Remove(c.path)
	s.storageBytes -= c.bytes
	c.Released = true
	delete(s.commands, token)
	s.JournalLocked("COMMAND_RELEASE", map[string]any{"cmd": token})
}

func (s *Session) Read(token, cursor string, maxBytes, maxEvents int) ([]map[string]any, string, bool, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	c := s.commands[token]
	if c == nil {
		return nil, "", false, errors.New("NOT_FOUND")
	}
	start := s.floorEvent
	if cursor != "" {
		seq, err := parseCursor(cursor)
		if err != nil {
			return nil, "", false, errors.New("INVALID_ARGUMENT")
		}
		if seq < s.floorEvent {
			return nil, cursorFor(s.floorEvent), false, errors.New("CURSOR_EXPIRED")
		}
		start = seq
	}
	if maxBytes <= 0 || maxBytes > 64*1024 {
		maxBytes = 64 * 1024
	}
	if maxEvents <= 0 || maxEvents > 256 {
		maxEvents = 256
	}
	out := []map[string]any{}
	used := 0
	next := start
	for _, ev := range c.Events {
		if ev.Seq < start {
			continue
		}
		if len(out) >= maxEvents || (used > 0 && used+len(ev.Data) > maxBytes) {
			break
		}
		item := eventForClient(ev)
		out = append(out, item)
		used += len(ev.Data)
		next = ev.Seq + 1
	}
	atEnd := true
	for _, ev := range c.Events {
		if ev.Seq >= next {
			atEnd = false
			break
		}
	}
	return out, cursorFor(next), atEnd, nil
}

func (s *Session) InitialCursor() string {
	s.mu.Lock()
	defer s.mu.Unlock()
	return cursorFor(s.nextEvent + 1)
}

func (s *Session) appendEventLocked(c *Command, ev Event) error {
	s.nextEvent++
	ev.Seq = s.nextEvent
	c.Events = append(c.Events, ev)
	if c.file != nil {
		var hdr [transcriptRecordHeader]byte
		binary.LittleEndian.PutUint64(hdr[0:8], ev.Seq)
		hdr[8] = ev.Kind
		binary.LittleEndian.PutUint32(hdr[9:13], uint32(len(ev.Data)))
		binary.LittleEndian.PutUint32(hdr[13:17], uint32(len(ev.State)))
		if _, err := c.file.Write(hdr[:]); err != nil {
			return err
		}
		if _, err := c.file.WriteString(ev.State); err != nil {
			return err
		}
		if _, err := c.file.Write(ev.Data); err != nil {
			return err
		}
		added := int64(len(hdr) + len(ev.State) + len(ev.Data))
		c.bytes += added
		s.storageBytes += added
	}
	if ev.Kind == eventStdout || ev.Kind == eventStderr {
		s.JournalLocked(strings.ToUpper(ev.Stream), map[string]any{"cmd": c.Token, "summary": journalPreview(ev.Data)})
	}
	s.enforceBudgetLocked()
	return nil
}

func (s *Session) enforceBudgetLocked() {
	limit := s.MaxBytes - DefaultJournalTotal
	if limit < 1024*1024 {
		limit = s.MaxBytes / 2
	}
	for s.storageBytes > limit {
		var oldest *Command
		for _, c := range s.commands {
			if len(c.Events) == 0 {
				continue
			}
			if oldest == nil || c.Events[0].Seq < oldest.Events[0].Seq {
				oldest = c
			}
		}
		if oldest == nil {
			return
		}
		ev := oldest.Events[0]
		oldest.Events = oldest.Events[1:]
		reclaimed := int64(transcriptRecordHeader + len(ev.State) + len(ev.Data))
		oldest.bytes -= reclaimed
		s.storageBytes -= reclaimed
		if ev.Seq+1 > s.floorEvent {
			oldFloor := s.floorEvent
			s.floorEvent = ev.Seq + 1
			s.JournalLocked("TRANSCRIPT_TRUNCATED", map[string]any{"old_floor": cursorFor(oldFloor), "new_floor": cursorFor(s.floorEvent), "bytes_reclaimed": reclaimed})
		}
	}
}

func (s *Session) openJournal() error {
	f, err := os.OpenFile(filepath.Join(s.Dir, "session.log"), os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0600)
	if err != nil {
		return err
	}
	s.journal = f
	return nil
}

func (s *Session) Journal(event string, fields map[string]any) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.JournalLocked(event, fields)
}

func (s *Session) JournalLocked(event string, fields map[string]any) {
	if s.journal == nil {
		return
	}
	row := map[string]any{"ts": time.Now().Format(time.RFC3339Nano), "session": s.ID, "event": event}
	for k, v := range fields {
		row[k] = v
	}
	b, err := json.Marshal(row)
	if err != nil {
		return
	}
	b = append(b, '\n')
	if s.journalBytes+int64(len(b)) > s.JournalSegment {
		_ = s.journal.Close()
		_ = os.Remove(filepath.Join(s.Dir, "session.log.1"))
		_ = os.Rename(filepath.Join(s.Dir, "session.log"), filepath.Join(s.Dir, "session.log.1"))
		_ = s.openJournal()
		s.journalBytes = 0
	}
	if _, err := s.journal.Write(b); err == nil {
		s.journalBytes += int64(len(b))
	}
}

func CommandStatus(c *Command) map[string]any {
	out := map[string]any{
		"command_token": c.Token,
		"kind":          c.Kind,
		"state":         c.State,
		"exit_code":     c.ExitCode,
		"signal":        c.Signal,
		"start_errno":   c.StartErrno,
		"lost_reason":   c.LostReason,
		"output_done":   c.OutputDone,
		"released":      c.Released,
		"execution_id":  strconv.FormatUint(c.TargetID, 10),
		"exec_ref":      strconv.FormatUint(c.ExecRef, 10),
	}
	if !c.LastVerified.IsZero() {
		out["last_verified_at"] = c.LastVerified.UTC().Format(time.RFC3339Nano)
	}
	return out
}

func cursorFor(seq uint64) string {
	var b [8]byte
	binary.BigEndian.PutUint64(b[:], seq)
	return "c:" + base64.RawURLEncoding.EncodeToString(b[:])
}

func parseCursor(s string) (uint64, error) {
	if !strings.HasPrefix(s, "c:") {
		return 0, errors.New("bad cursor")
	}
	b, err := base64.RawURLEncoding.DecodeString(strings.TrimPrefix(s, "c:"))
	if err != nil || len(b) != 8 {
		return 0, errors.New("bad cursor")
	}
	return binary.BigEndian.Uint64(b), nil
}

func eventForClient(ev Event) map[string]any {
	m := map[string]any{"cursor": cursorFor(ev.Seq), "event": eventName(ev.Kind)}
	if ev.Stream != "" {
		m["stream"] = ev.Stream
	}
	if ev.State != "" {
		m["state"] = ev.State
	}
	if ev.Kind == eventStdout || ev.Kind == eventStderr {
		m["bytes"] = len(ev.Data)
		if safeText(ev.Data) {
			m["text"] = string(ev.Data)
		} else {
			m["base64"] = base64.StdEncoding.EncodeToString(ev.Data)
		}
	} else if len(ev.Data) > 0 {
		m["data"] = string(ev.Data)
	}
	return m
}

func eventName(k byte) string {
	switch k {
	case eventStdout:
		return "output"
	case eventStderr:
		return "output"
	case eventState:
		return "state"
	case eventLost:
		return "lost"
	default:
		return "unknown"
	}
}

func stateName(st byte) string {
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

func safeText(b []byte) bool {
	if !utf8.Valid(b) {
		return false
	}
	for _, r := range string(b) {
		if r < 0x20 && r != '\n' && r != '\r' && r != '\t' {
			return false
		}
	}
	return true
}

func journalPreview(b []byte) any {
	if safeText(b) {
		s := string(b)
		if len(s) > 160 {
			s = s[:160]
		}
		return s
	}
	n := len(b)
	if n > 24 {
		n = 24
	}
	return map[string]any{"binary_bytes": len(b), "hex_preview": hex.EncodeToString(b[:n])}
}

func safeName(s string) string {
	var b strings.Builder
	for _, r := range s {
		if (r >= 'a' && r <= 'z') || (r >= 'A' && r <= 'Z') || (r >= '0' && r <= '9') || r == '-' || r == '_' {
			b.WriteRune(r)
		} else {
			b.WriteByte('_')
		}
	}
	return b.String()
}

func randomHex(n int) string {
	b := make([]byte, n)
	if _, err := rand.Read(b); err != nil {
		now := uint64(time.Now().UnixNano())
		for i := range b {
			b[i] = byte(now >> (uint(i%8) * 8))
		}
	}
	return hex.EncodeToString(b)
}
