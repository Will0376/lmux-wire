package mcpapp

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"lmux/internal/hc2"
)

func TestTranscriptCursorAndExpiration(t *testing.T) {
	dir := t.TempDir()
	s := &Session{ID: "s", Dir: dir, MaxBytes: 1024, JournalSegment: 256, commands: map[string]*Command{}, floorEvent: 1}
	if err := s.openJournal(); err != nil {
		t.Fatal(err)
	}
	c, err := s.NewCommand([]string{"/bin/echo"}, "", 1, 1, "")
	if err != nil {
		t.Fatal(err)
	}
	start := s.InitialCursor()
	if err := s.AppendOutput(c.Token, "stdout", []byte("hello")); err != nil {
		t.Fatal(err)
	}
	items, next, atEnd, err := s.Read(c.Token, start, 100, 10)
	if err != nil || len(items) != 1 || !atEnd || next == start {
		t.Fatalf("bad read items=%v next=%s atEnd=%v err=%v", items, next, atEnd, err)
	}
	for i := 0; i < 80; i++ {
		if err := s.AppendOutput(c.Token, "stdout", []byte("xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx")); err != nil {
			t.Fatal(err)
		}
	}
	_, earliest, _, err := s.Read(c.Token, start, 100, 10)
	if err == nil || err.Error() != "CURSOR_EXPIRED" || earliest == "" {
		t.Fatalf("want cursor expired, earliest=%s err=%v", earliest, err)
	}
}

func TestCommandKindAndActiveCounts(t *testing.T) {
	dir := t.TempDir()
	s := &Session{ID: "s", Dir: dir, MaxBytes: 4096, JournalSegment: 256, commands: map[string]*Command{}, floorEvent: 1}
	if err := s.openJournal(); err != nil {
		t.Fatal(err)
	}
	for i := 0; i < DefaultMaxSessionCommands; i++ {
		if _, err := s.NewCommand([]string{"/bin/sleep"}, "", uint64(i+1), uint64(i+1), "session"); err != nil {
			t.Fatal(err)
		}
	}
	sessionCount, totalCount := s.ActiveCommandCounts()
	if sessionCount != DefaultMaxSessionCommands || totalCount != DefaultMaxSessionCommands {
		t.Fatalf("counts session=%d total=%d", sessionCount, totalCount)
	}
	one, err := s.NewCommand([]string{"/bin/true"}, "", 10, 10, "")
	if err != nil {
		t.Fatal(err)
	}
	if one.Kind != "oneshot" {
		t.Fatalf("default kind = %q", one.Kind)
	}
	sessionCount, totalCount = s.ActiveCommandCounts()
	if sessionCount != DefaultMaxSessionCommands || totalCount != DefaultMaxConcurrentCommand {
		t.Fatalf("counts after oneshot session=%d total=%d", sessionCount, totalCount)
	}
	s.Release(one.Token)
	sessionCount, totalCount = s.ActiveCommandCounts()
	if sessionCount != DefaultMaxSessionCommands || totalCount != DefaultMaxSessionCommands {
		t.Fatalf("counts after release session=%d total=%d", sessionCount, totalCount)
	}
	if _, err := NormalizeCommandKind("forever"); err == nil {
		t.Fatal("invalid command kind accepted")
	}
}

func TestCommandListingScopeTerminalReleaseAndBound(t *testing.T) {
	dir := t.TempDir()
	s := &Session{ID: "scope-a", Dir: dir, MaxBytes: 4096, JournalSegment: 256, commands: map[string]*Command{}, floorEvent: 1}
	if err := s.openJournal(); err != nil {
		t.Fatal(err)
	}
	running, err := s.NewCommand([]string{"/bin/sleep"}, "", 11, ^uint64(0), "session")
	if err != nil {
		t.Fatal(err)
	}
	if err := s.UpdateState(running.Token, hc2.ExecState{State: 2}); err != nil {
		t.Fatal(err)
	}
	terminating, err := s.NewCommand([]string{"/bin/sleep"}, "", 13, 98, "oneshot")
	if err != nil {
		t.Fatal(err)
	}
	if err := s.UpdateState(terminating.Token, hc2.ExecState{State: 6}); err != nil {
		t.Fatal(err)
	}
	done, err := s.NewCommand([]string{"/bin/true"}, "", 12, 99, "oneshot")
	if err != nil {
		t.Fatal(err)
	}
	if err := s.UpdateState(done.Token, hc2.ExecState{State: 4}); err != nil {
		t.Fatal(err)
	}
	items, total := s.ListCommands(false, 128)
	if total != 2 || len(items) != 2 || items[0].Token != running.Token ||
		items[1].Token != terminating.Token ||
		items[0].TargetID != ^uint64(0) || items[0].LastVerified.IsZero() {
		t.Fatalf("live listing = %#v total=%d", items, total)
	}
	items, total = s.ListCommands(true, 1)
	if total != 3 || len(items) != 1 || items[0].Token != done.Token {
		t.Fatalf("bounded terminal listing = %#v total=%d", items, total)
	}
	s.MarkLostByRef(running.ExecRef, hc2.ErrStaleTarget)
	items, total = s.ListCommands(false, 128)
	if total != 1 || len(items) != 1 || items[0].Token != terminating.Token {
		t.Fatalf("LOST command appeared in default listing: %#v total=%d", items, total)
	}
	s.Release(done.Token)
	items, total = s.ListCommands(true, 128)
	if total != 2 || len(items) != 2 || items[0].Token != running.Token ||
		time.Since(items[0].LastVerified) > time.Second {
		t.Fatalf("released token/list state = %#v total=%d", items, total)
	}
}

func TestManagerRestartDoesNotRestoreCommandTokens(t *testing.T) {
	dir := t.TempDir()
	m := NewManager(dir)
	s, err := m.Start()
	if err != nil {
		t.Fatal(err)
	}
	if _, err := s.NewCommand([]string{"/bin/sleep"}, "", 1, 2, "session"); err != nil {
		t.Fatal(err)
	}
	m.Close()

	m2 := NewManager(dir)
	s2, err := m2.Start()
	if err != nil {
		t.Fatal(err)
	}
	defer m2.Close()
	items, total := s2.ListCommands(true, 128)
	if total != 0 || len(items) != 0 || s2.ID == s.ID {
		t.Fatalf("fresh MCP process session restored old tokens: old=%q new=%q items=%#v", s.ID, s2.ID, items)
	}
}

func TestJournalRotation(t *testing.T) {
	dir := t.TempDir()
	s := &Session{ID: "s", Dir: dir, MaxBytes: 4096, JournalSegment: 128, commands: map[string]*Command{}, floorEvent: 1}
	if err := s.openJournal(); err != nil {
		t.Fatal(err)
	}
	for i := 0; i < 20; i++ {
		s.Journal("STDOUT", map[string]any{"text": "abcdefghijklmnopqrstuvwxyz"})
	}
	if _, err := os.Stat(filepath.Join(dir, "session.log")); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(filepath.Join(dir, "session.log.1")); err != nil {
		t.Fatal(err)
	}
}

func TestManagerStartupCleanupAndLock(t *testing.T) {
	dir := t.TempDir()
	if err := os.MkdirAll(filepath.Join(dir, "sessions", "session-old"), 0700); err != nil {
		t.Fatal(err)
	}
	m := NewManager(dir)
	m.MaxSessions = 1
	s, err := m.Start()
	if err != nil {
		t.Fatal(err)
	}
	if s == nil {
		t.Fatal("nil session")
	}
	m2 := NewManager(dir)
	if _, err := m2.Start(); err == nil {
		t.Fatal("second instance acquired lock")
	}
	m.Close()
}

func TestManagerDefaultRuntimeUsesXDG(t *testing.T) {
	xdg := t.TempDir()
	t.Setenv("XDG_RUNTIME_DIR", xdg)
	m := NewManager("")
	s, err := m.Start()
	if err != nil {
		t.Fatal(err)
	}
	defer m.Close()
	want := filepath.Join(xdg, "lmux", "mcp")
	if m.RuntimeDir != want {
		t.Fatalf("runtime dir = %q, want %q", m.RuntimeDir, want)
	}
	if !strings.HasPrefix(s.Dir, filepath.Join(want, "sessions")+string(os.PathSeparator)) {
		t.Fatalf("session dir %q not under %q", s.Dir, want)
	}
	if _, err := os.Stat(filepath.Join(want, "current", "session.log")); err != nil {
		t.Fatalf("journal not discoverable under XDG runtime: %v", err)
	}
}

func TestManagerRuntimeOverride(t *testing.T) {
	t.Setenv("XDG_RUNTIME_DIR", "")
	dir := filepath.Join(t.TempDir(), "explicit-mcp")
	m := NewManager(dir)
	s, err := m.Start()
	if err != nil {
		t.Fatal(err)
	}
	defer m.Close()
	if !strings.HasPrefix(s.Dir, filepath.Join(dir, "sessions")+string(os.PathSeparator)) {
		t.Fatalf("session dir %q not under explicit runtime %q", s.Dir, dir)
	}
}

func TestManagerDefaultRuntimeRequiresXDG(t *testing.T) {
	t.Setenv("XDG_RUNTIME_DIR", "")
	m := NewManager("")
	if _, err := m.Start(); err == nil || !strings.Contains(err.Error(), "XDG_RUNTIME_DIR") {
		t.Fatalf("want XDG_RUNTIME_DIR error, got %v", err)
	}
	if m.RuntimeDir == "/run/lmux/mcp" {
		t.Fatal("default runtime must not fall back to /run/lmux/mcp")
	}
}
