package gdbmi

import (
	"bufio"
	"context"
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"time"
)

const DefaultCommandTimeout = 10 * time.Second

type Event struct {
	Record Record
}

type Process struct {
	cmd    *exec.Cmd
	stdin  io.WriteCloser
	cancel context.CancelFunc

	parser *Parser
	events chan Event

	next atomic.Uint64

	mu               sync.Mutex
	pending          map[int]chan Record
	closed           bool
	err              error
	diag             tail
	lastTargetSelect []byte
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

func Start(ctx context.Context, path string, args ...string) (*Process, error) {
	return start(ctx, nil, path, args...)
}

func StartEnv(ctx context.Context, env []string, path string, args ...string) (*Process, error) {
	return start(ctx, env, path, args...)
}

func start(ctx context.Context, env []string, path string, args ...string) (*Process, error) {
	if path == "" {
		return nil, errors.New("GDB_NOT_CONFIGURED")
	}
	cctx, cancel := context.WithCancel(ctx)
	cmd := exec.CommandContext(cctx, path, args...)
	if env != nil {
		cmd.Env = env
	}
	stdout, err := cmd.StdoutPipe()
	if err != nil {
		cancel()
		return nil, err
	}
	stderr, err := cmd.StderrPipe()
	if err != nil {
		cancel()
		return nil, err
	}
	stdin, err := cmd.StdinPipe()
	if err != nil {
		cancel()
		return nil, err
	}
	if err := cmd.Start(); err != nil {
		cancel()
		return nil, err
	}
	p := &Process{
		cmd:     cmd,
		stdin:   stdin,
		cancel:  cancel,
		parser:  NewParser(DefaultMaxRecord),
		events:  make(chan Event, 64),
		pending: make(map[int]chan Record),
	}
	go p.readStdout(stdout)
	go p.readStderr(stderr)
	go func() {
		err := cmd.Wait()
		cancel()
		if err == nil {
			err = errors.New("process exited")
		}
		p.closeWith(p.annotateError("GDB_EXITED", err))
	}()
	return p, nil
}

func StartCommand(ctx context.Context, path string, args ...string) (*Process, error) {
	return Start(ctx, path, args...)
}

func IsFakeHelper() bool {
	return os.Getenv("LMUX_GDBMI_FAKE_HELPER") == "1"
}

func StartGDB(ctx context.Context, path string) (*Process, error) {
	return Start(ctx, path, "--quiet", "--nx", "--nh", "--interpreter=mi2")
}

func (p *Process) Events() <-chan Event { return p.events }

func (p *Process) PID() int {
	if p == nil || p.cmd == nil || p.cmd.Process == nil {
		return 0
	}
	return p.cmd.Process.Pid
}

func (p *Process) Diagnostics() string {
	p.mu.Lock()
	defer p.mu.Unlock()
	return p.diag.string()
}

func (p *Process) Err() error {
	p.mu.Lock()
	defer p.mu.Unlock()
	return p.err
}

func (p *Process) LastTargetSelectBytes() []byte {
	p.mu.Lock()
	defer p.mu.Unlock()
	return append([]byte(nil), p.lastTargetSelect...)
}

func (p *Process) Close() error {
	p.cancel()
	if p.stdin != nil {
		_ = p.stdin.Close()
	}
	if p.cmd != nil && p.cmd.Process != nil {
		_ = p.cmd.Process.Kill()
	}
	return nil
}

func (p *Process) Command(ctx context.Context, command string) (Record, error) {
	return p.command(ctx, command, true)
}

func (p *Process) CommandNoDefaultTimeout(ctx context.Context, command string) (Record, error) {
	return p.command(ctx, command, false)
}

func (p *Process) command(ctx context.Context, command string, defaultTimeout bool) (Record, error) {
	if deadline, ok := ctx.Deadline(); !ok || time.Until(deadline) > DefaultCommandTimeout {
		if defaultTimeout {
			var cancel context.CancelFunc
			ctx, cancel = context.WithTimeout(ctx, DefaultCommandTimeout)
			defer cancel()
		}
	}
	token := int(p.next.Add(1))
	ch := make(chan Record, 1)
	p.mu.Lock()
	if p.closed {
		err := p.commandClosedErrorLocked("GDB_MI_WRITE_FAILED")
		p.mu.Unlock()
		return Record{}, err
	}
	p.pending[token] = ch
	p.mu.Unlock()
	line := strconv.Itoa(token) + command + "\n"
	if strings.HasPrefix(command, "-target-select ") {
		p.mu.Lock()
		p.lastTargetSelect = []byte(line)
		p.mu.Unlock()
	}
	if _, err := io.WriteString(p.stdin, line); err != nil {
		p.forget(token)
		return Record{}, p.annotateError("GDB_MI_WRITE_FAILED", err)
	}
	select {
	case <-ctx.Done():
		p.forget(token)
		return Record{}, ctx.Err()
	case rec, ok := <-ch:
		if !ok {
			p.mu.Lock()
			err := p.commandClosedErrorLocked("GDB_MI_READ_FAILED")
			p.mu.Unlock()
			return Record{}, err
		}
		if rec.Kind == Result && rec.Class == "error" {
			if msg, ok := rec.Result("msg"); ok {
				return rec, fmt.Errorf("GDB_ERROR: %s", msg.StringValue())
			}
			return rec, errors.New("GDB_ERROR")
		}
		return rec, nil
	}
}

func (p *Process) readStdout(r io.Reader) {
	br := bufio.NewReader(r)
	buf := make([]byte, 4096)
	for {
		n, err := br.Read(buf)
		if n > 0 {
			recs, perr := p.parser.Feed(buf[:n])
			if perr != nil {
				p.closeWith(p.annotateError("GDB_MI_READ_FAILED", perr))
				return
			}
			for _, rec := range recs {
				p.dispatch(rec)
			}
		}
		if err != nil {
			if err == io.EOF {
				if recs, perr := p.parser.EOF(); perr != nil {
					p.closeWith(p.annotateError("GDB_MI_READ_FAILED", perr))
				} else {
					for _, rec := range recs {
						p.dispatch(rec)
					}
					p.closeWith(p.annotateError("GDB_EXITED", io.EOF))
				}
			} else if errors.Is(err, os.ErrClosed) || strings.Contains(err.Error(), "file already closed") {
				p.closeWith(p.annotateError("GDB_EXITED", err))
			} else {
				p.closeWith(p.annotateError("GDB_MI_READ_FAILED", err))
			}
			return
		}
	}
}

func (p *Process) readStderr(r io.Reader) {
	buf := make([]byte, 2048)
	for {
		n, err := r.Read(buf)
		if n > 0 {
			p.mu.Lock()
			p.diag.add(buf[:n])
			p.mu.Unlock()
		}
		if err != nil {
			return
		}
	}
}

func (p *Process) dispatch(rec Record) {
	if rec.Kind == Result && rec.Token != 0 {
		p.mu.Lock()
		ch := p.pending[rec.Token]
		if ch != nil {
			delete(p.pending, rec.Token)
			ch <- rec
			close(ch)
			p.mu.Unlock()
			return
		}
		p.mu.Unlock()
		p.closeWith(fmt.Errorf("MI_UNEXPECTED_RESULT_TOKEN %d", rec.Token))
		return
	}
	if rec.Kind == ExecAsync || rec.Kind == NotifyAsync || rec.Kind == StatusAsync ||
		rec.Kind == ConsoleStream || rec.Kind == TargetStream || rec.Kind == LogStream {
		p.mu.Lock()
		if p.closed {
			p.mu.Unlock()
			return
		}
		select {
		case p.events <- Event{Record: rec}:
			p.mu.Unlock()
		default:
			p.mu.Unlock()
			p.closeWith(errors.New("MI_EVENT_QUEUE_FULL"))
		}
	}
}

func (p *Process) commandClosedErrorLocked(fallback string) error {
	err := p.err
	if err == nil {
		err = io.ErrClosedPipe
	}
	if strings.HasPrefix(err.Error(), "GDB_") {
		return err
	}
	msg := fmt.Sprintf("%s: %v", fallback, err)
	if diag := p.diag.string(); diag != "" {
		msg += fmt.Sprintf(` stderr=%q`, diag)
	}
	return errors.New(msg)
}

func (p *Process) annotateError(prefix string, err error) error {
	if err == nil {
		err = io.ErrClosedPipe
	}
	msg := fmt.Sprintf("%s: %v", prefix, err)
	if diag := p.Diagnostics(); diag != "" {
		msg += fmt.Sprintf(` stderr=%q`, diag)
	}
	return errors.New(msg)
}

func (p *Process) forget(token int) {
	p.mu.Lock()
	delete(p.pending, token)
	p.mu.Unlock()
}

func (p *Process) closeWith(err error) {
	p.mu.Lock()
	if p.closed {
		p.mu.Unlock()
		return
	}
	p.closed = true
	if err == nil {
		err = io.EOF
	}
	if !strings.HasPrefix(err.Error(), "GDB_") && strings.Contains(err.Error(), "signal: killed") {
		err = io.ErrClosedPipe
	}
	p.err = err
	for tok, ch := range p.pending {
		delete(p.pending, tok)
		close(ch)
	}
	p.mu.Unlock()
	close(p.events)
}
