package gdbmi

import (
	"errors"
	"fmt"
	"strconv"
	"strings"
)

const DefaultMaxRecord = 64 * 1024

type RecordKind int

const (
	Result RecordKind = iota + 1
	ExecAsync
	StatusAsync
	NotifyAsync
	ConsoleStream
	TargetStream
	LogStream
	Prompt
)

type Record struct {
	Token   int
	Kind    RecordKind
	Class   string
	Results []ResultKV
	Stream  string
}

type ResultKV struct {
	Key   string
	Value Value
}

type Value struct {
	Kind  ValueKind
	Str   string
	Tuple []ResultKV
	List  []Value
}

type ValueKind int

const (
	String ValueKind = iota + 1
	Tuple
	List
)

type Parser struct {
	max int
	buf []byte
}

func NewParser(max int) *Parser {
	if max <= 0 {
		max = DefaultMaxRecord
	}
	return &Parser{max: max}
}

func (p *Parser) Feed(data []byte) ([]Record, error) {
	var out []Record
	for _, b := range data {
		if len(p.buf) >= p.max {
			return out, errors.New("MI_RECORD_TOO_LARGE")
		}
		if b == '\n' {
			line := strings.TrimSuffix(string(p.buf), "\r")
			p.buf = p.buf[:0]
			if line == "" {
				continue
			}
			rec, ok, err := ParseRecord(line)
			if err != nil {
				return out, err
			}
			if ok {
				out = append(out, rec)
			}
			continue
		}
		p.buf = append(p.buf, b)
	}
	return out, nil
}

func (p *Parser) EOF() ([]Record, error) {
	if len(p.buf) == 0 {
		return nil, nil
	}
	line := strings.TrimSuffix(string(p.buf), "\r")
	p.buf = nil
	rec, ok, err := ParseRecord(line)
	if err != nil {
		return nil, err
	}
	if !ok {
		return nil, nil
	}
	return []Record{rec}, nil
}

func ParseRecord(line string) (Record, bool, error) {
	if strings.TrimSpace(line) == "(gdb)" {
		return Record{Kind: Prompt}, true, nil
	}
	var rec Record
	i := 0
	for i < len(line) && line[i] >= '0' && line[i] <= '9' {
		i++
	}
	if i > 0 {
		tok, err := strconv.Atoi(line[:i])
		if err != nil {
			return rec, false, err
		}
		rec.Token = tok
	}
	if i >= len(line) {
		return rec, false, errors.New("MI_MISSING_RECORD_KIND")
	}
	switch line[i] {
	case '^':
		rec.Kind = Result
	case '*':
		rec.Kind = ExecAsync
	case '+':
		rec.Kind = StatusAsync
	case '=':
		rec.Kind = NotifyAsync
	case '~', '@', '&':
		if rec.Token != 0 {
			return rec, false, errors.New("MI_STREAM_HAS_TOKEN")
		}
		switch line[i] {
		case '~':
			rec.Kind = ConsoleStream
		case '@':
			rec.Kind = TargetStream
		case '&':
			rec.Kind = LogStream
		}
		s := scanner{s: line[i+1:]}
		v, err := s.parseCString()
		if err != nil {
			return rec, false, err
		}
		if !s.end() {
			return rec, false, errors.New("MI_TRAILING_STREAM_DATA")
		}
		rec.Stream = v
		return rec, true, nil
	default:
		return rec, false, fmt.Errorf("MI_UNKNOWN_RECORD_KIND %q", line[i])
	}
	s := scanner{s: line[i+1:]}
	cls, err := s.parseIdent()
	if err != nil {
		return rec, false, err
	}
	rec.Class = cls
	if s.peek(',') {
		s.pos++
		rec.Results, err = s.parseResultList(0)
		if err != nil {
			return rec, false, err
		}
	}
	if !s.end() {
		return rec, false, errors.New("MI_TRAILING_DATA")
	}
	return rec, true, nil
}

func (r Record) Result(key string) (Value, bool) {
	for _, kv := range r.Results {
		if kv.Key == key {
			return kv.Value, true
		}
	}
	return Value{}, false
}

func (v Value) StringValue() string {
	if v.Kind != String {
		return ""
	}
	return v.Str
}

type scanner struct {
	s   string
	pos int
}

func (s *scanner) end() bool { return s.pos == len(s.s) }

func (s *scanner) peek(ch byte) bool {
	return s.pos < len(s.s) && s.s[s.pos] == ch
}

func (s *scanner) parseIdent() (string, error) {
	start := s.pos
	for s.pos < len(s.s) {
		c := s.s[s.pos]
		if (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
			(c >= '0' && c <= '9') || c == '-' || c == '_' {
			s.pos++
			continue
		}
		break
	}
	if start == s.pos {
		return "", errors.New("MI_EXPECTED_IDENTIFIER")
	}
	return s.s[start:s.pos], nil
}

func (s *scanner) parseResultList(term byte) ([]ResultKV, error) {
	var out []ResultKV
	for {
		if term != 0 && s.peek(term) {
			return out, nil
		}
		key, err := s.parseIdent()
		if err != nil {
			return nil, err
		}
		if !s.peek('=') {
			return nil, errors.New("MI_EXPECTED_EQUALS")
		}
		s.pos++
		val, err := s.parseValue()
		if err != nil {
			return nil, err
		}
		out = append(out, ResultKV{Key: key, Value: val})
		if !s.peek(',') {
			return out, nil
		}
		s.pos++
	}
}

func (s *scanner) parseValue() (Value, error) {
	if s.end() {
		return Value{}, errors.New("MI_EXPECTED_VALUE")
	}
	switch s.s[s.pos] {
	case '"':
		str, err := s.parseCString()
		return Value{Kind: String, Str: str}, err
	case '{':
		s.pos++
		if s.peek('}') {
			s.pos++
			return Value{Kind: Tuple}, nil
		}
		items, err := s.parseResultList('}')
		if err != nil {
			return Value{}, err
		}
		if !s.peek('}') {
			return Value{}, errors.New("MI_EXPECTED_TUPLE_END")
		}
		s.pos++
		return Value{Kind: Tuple, Tuple: items}, nil
	case '[':
		return s.parseList()
	default:
		return Value{}, errors.New("MI_EXPECTED_VALUE")
	}
}

func (s *scanner) parseList() (Value, error) {
	s.pos++
	if s.peek(']') {
		s.pos++
		return Value{Kind: List}, nil
	}
	var vals []Value
	for {
		if s.pos < len(s.s) {
			save := s.pos
			if _, err := s.parseIdent(); err == nil && s.peek('=') {
				s.pos = save
				items, err := s.parseResultList(0)
				if err != nil {
					return Value{}, err
				}
				vals = append(vals, Value{Kind: Tuple, Tuple: items})
			} else {
				s.pos = save
				v, err := s.parseValue()
				if err != nil {
					return Value{}, err
				}
				vals = append(vals, v)
			}
		}
		if s.peek(']') {
			s.pos++
			return Value{Kind: List, List: vals}, nil
		}
		if !s.peek(',') {
			return Value{}, errors.New("MI_EXPECTED_LIST_SEPARATOR")
		}
		s.pos++
	}
}

func (s *scanner) parseCString() (string, error) {
	if !s.peek('"') {
		return "", errors.New("MI_EXPECTED_STRING")
	}
	s.pos++
	var b strings.Builder
	for s.pos < len(s.s) {
		ch := s.s[s.pos]
		s.pos++
		if ch == '"' {
			return b.String(), nil
		}
		if ch != '\\' {
			b.WriteByte(ch)
			continue
		}
		if s.pos >= len(s.s) {
			return "", errors.New("MI_BAD_ESCAPE")
		}
		esc := s.s[s.pos]
		s.pos++
		switch esc {
		case 'n':
			b.WriteByte('\n')
		case 'r':
			b.WriteByte('\r')
		case 't':
			b.WriteByte('\t')
		case 'b':
			b.WriteByte('\b')
		case 'f':
			b.WriteByte('\f')
		case '\\', '"':
			b.WriteByte(esc)
		default:
			if esc >= '0' && esc <= '7' {
				val := int(esc - '0')
				for i := 0; i < 2 && s.pos < len(s.s); i++ {
					c := s.s[s.pos]
					if c < '0' || c > '7' {
						break
					}
					s.pos++
					val = val*8 + int(c-'0')
				}
				b.WriteByte(byte(val))
			} else {
				b.WriteByte(esc)
			}
		}
	}
	return "", errors.New("MI_UNTERMINATED_STRING")
}
