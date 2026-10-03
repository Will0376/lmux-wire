package mcpapp

import (
	"os"
	"path/filepath"
	"testing"
)

func TestTranscriptCursorAndExpiration(t *testing.T) {
	dir := t.TempDir()
	s := &Session{ID: "s", Dir: dir, MaxBytes: 1024, JournalSegment: 256, commands: map[string]*Command{}, floorEvent: 1}
	if err := s.openJournal(); err != nil {
		t.Fatal(err)
	}
	c, err := s.NewCommand([]string{"/bin/echo"}, "", 1, 1)
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
