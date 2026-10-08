package gdbmi

import (
	"strings"
	"testing"
)

func TestParseRecords(t *testing.T) {
	tests := []struct {
		line  string
		kind  RecordKind
		token int
		class string
	}{
		{`1^done`, Result, 1, "done"},
		{`2^error,msg="bad \"thing\""`, Result, 2, "error"},
		{`3^running`, Result, 3, "running"},
		{`*running,thread-id="all"`, ExecAsync, 0, "running"},
		{`*stopped,reason="breakpoint-hit",frame={addr="0x1",func="foo",args=[]}`, ExecAsync, 0, "stopped"},
		{`=thread-created,id="1",group-id="i1"`, NotifyAsync, 0, "thread-created"},
		{`~"console\n"`, ConsoleStream, 0, ""},
		{`@"inferior\n"`, TargetStream, 0, ""},
		{`&"log\n"`, LogStream, 0, ""},
		{`(gdb)`, Prompt, 0, ""},
	}
	for _, tt := range tests {
		rec, ok, err := ParseRecord(tt.line)
		if err != nil || !ok {
			t.Fatalf("%q parse ok=%v err=%v", tt.line, ok, err)
		}
		if rec.Kind != tt.kind || rec.Token != tt.token || rec.Class != tt.class {
			t.Fatalf("%q got kind=%v token=%d class=%q", tt.line, rec.Kind, rec.Token, rec.Class)
		}
	}
}

func TestNestedValuesAndEscapes(t *testing.T) {
	rec, ok, err := ParseRecord(`4^done,bkpt={number="1",addr="0x100",loc=["a","b"],meta={file="x\\y",line="7"}},empty=[],list=[frame={level="0",func="main"}]`)
	if err != nil || !ok {
		t.Fatalf("parse ok=%v err=%v", ok, err)
	}
	bkpt, ok := rec.Result("bkpt")
	if !ok || bkpt.Kind != Tuple || len(bkpt.Tuple) != 4 {
		t.Fatalf("bad bkpt: %#v", bkpt)
	}
	if bkpt.Tuple[3].Value.Tuple[0].Value.Str != `x\y` {
		t.Fatalf("bad escaped path: %q", bkpt.Tuple[3].Value.Tuple[0].Value.Str)
	}
	empty, ok := rec.Result("empty")
	if !ok || empty.Kind != List || len(empty.List) != 0 {
		t.Fatalf("bad empty list: %#v", empty)
	}
}

func TestParserFragmentedCoalescedAndEOF(t *testing.T) {
	p := NewParser(1024)
	var got []Record
	input := []byte("1^done\n2^error,msg=\"x\"\n~\"tail\"\n")
	for _, b := range input {
		recs, err := p.Feed([]byte{b})
		if err != nil {
			t.Fatal(err)
		}
		got = append(got, recs...)
	}
	if len(got) != 3 {
		t.Fatalf("fragmented records=%d", len(got))
	}
	p = NewParser(1024)
	got, err := p.Feed(input)
	if err != nil {
		t.Fatal(err)
	}
	if len(got) != 3 {
		t.Fatalf("coalesced records=%d", len(got))
	}
	p = NewParser(1024)
	if _, err := p.Feed([]byte("5^done")); err != nil {
		t.Fatal(err)
	}
	got, err = p.EOF()
	if err != nil || len(got) != 1 || got[0].Token != 5 {
		t.Fatalf("EOF got=%#v err=%v", got, err)
	}
}

func TestParserPromptSequences(t *testing.T) {
	tests := []struct {
		name  string
		input string
		kinds []RecordKind
	}{
		{name: "prompt only", input: "(gdb)\n", kinds: []RecordKind{Prompt}},
		{name: "prompt with trailing space", input: "(gdb) \n", kinds: []RecordKind{Prompt}},
		{name: "result prompt", input: "1^done\n(gdb)\n", kinds: []RecordKind{Result, Prompt}},
		{name: "stream prompt", input: "~\"banner text\\n\"\n(gdb)\n", kinds: []RecordKind{ConsoleStream, Prompt}},
		{name: "quoted prompt value", input: "1^done,value=\"(gdb)\"\n(gdb)\n", kinds: []RecordKind{Result, Prompt}},
		{name: "initial startup prompt", input: "=thread-group-added,id=\"i1\"\n(gdb)\n", kinds: []RecordKind{NotifyAsync, Prompt}},
		{name: "multiple command cycles", input: "1^done\n(gdb)\n2^done\n(gdb)\n", kinds: []RecordKind{Result, Prompt, Result, Prompt}},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			p := NewParser(1024)
			recs, err := p.Feed([]byte(tt.input))
			if err != nil {
				t.Fatal(err)
			}
			if len(recs) != len(tt.kinds) {
				t.Fatalf("records=%d want %d: %#v", len(recs), len(tt.kinds), recs)
			}
			for i, kind := range tt.kinds {
				if recs[i].Kind != kind {
					t.Fatalf("record %d kind=%v want %v", i, recs[i].Kind, kind)
				}
			}
			if tt.name == "quoted prompt value" {
				v, ok := recs[0].Result("value")
				if !ok || v.StringValue() != "(gdb)" {
					t.Fatalf("quoted prompt value parsed incorrectly: %#v", recs[0])
				}
			}
		})
	}
}

func TestParserFragmentedPrompt(t *testing.T) {
	p := NewParser(1024)
	var got []Record
	for _, chunk := range []string{"(", "gd", "b", ")", " \r", "\n"} {
		recs, err := p.Feed([]byte(chunk))
		if err != nil {
			t.Fatal(err)
		}
		got = append(got, recs...)
	}
	if len(got) != 1 || got[0].Kind != Prompt {
		t.Fatalf("fragmented prompt got %#v", got)
	}
}

func TestMalformedAndOversize(t *testing.T) {
	bad := []string{
		`^`,
		`1^done,`,
		`1^done,a`,
		`1^done,a=`,
		`1^done,a={x="y"`,
		`~"unterminated`,
	}
	for _, line := range bad {
		if _, _, err := ParseRecord(line); err == nil {
			t.Fatalf("expected error for %q", line)
		}
	}
	p := NewParser(4)
	if _, err := p.Feed([]byte("12345")); err == nil || !strings.Contains(err.Error(), "TOO_LARGE") {
		t.Fatalf("expected oversize, got %v", err)
	}
	p = NewParser(1024)
	if _, err := p.Feed([]byte(`~"unterminated`)); err != nil {
		t.Fatal(err)
	}
	if _, err := p.EOF(); err == nil {
		t.Fatal("expected EOF incomplete error")
	}
}
