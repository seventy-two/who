// who-engine is Who's connection to WhatsApp: whatsmeow (signed in as a
// linked device, like WhatsApp Web) plus a message store. The window runs
// it hidden and talks to it with JSON lines on stdin and stdout: commands
// in, events out (see protocol.go).
package main

import (
	"bufio"
	"context"
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"sync"
	"time"

	"go.mau.fi/whatsmeow"
	"go.mau.fi/whatsmeow/store"
	"go.mau.fi/whatsmeow/store/sqlstore"
	waLog "go.mau.fi/whatsmeow/util/log"
	"google.golang.org/protobuf/proto"
)

func main() {
	dataDir := flag.String("data", "", "folder for the session, messages and media")
	flag.Parse()
	if *dataDir == "" {
		fmt.Fprintln(os.Stderr, "who-engine: -data is required")
		os.Exit(2)
	}
	os.MkdirAll(filepath.Join(*dataDir, "media"), 0o700)

	logFile, _ := os.OpenFile(filepath.Join(*dataDir, "engine.log"), os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0o600)
	if logFile == nil {
		logFile = os.Stderr
	}
	// Never log to stdout: it carries the events.
	log := newFileLog(logFile, "INFO")

	e := &Engine{out: bufio.NewWriter(os.Stdout), dataDir: *dataDir, log: log}
	if err := e.Start(); err != nil {
		e.Emit(map[string]any{"t": "fatal", "error": err.Error()})
		log.Errorf("start: %v", err)
		os.Exit(1)
	}
	// Commands until the window closes our stdin, then a clean disconnect.
	in := bufio.NewScanner(os.Stdin)
	in.Buffer(make([]byte, 1<<20), 64<<20)
	for in.Scan() {
		var cmd map[string]any
		if err := json.Unmarshal(in.Bytes(), &cmd); err != nil {
			continue
		}
		go e.Handle(cmd)
	}
	e.Stop()
}

// Engine holds the WhatsApp client and the store.
type Engine struct {
	outMu   sync.Mutex
	out     *bufio.Writer
	dataDir string
	log     waLog.Logger

	cli   *whatsmeow.Client
	db    *Store
	names *Names
	ctx   context.Context

	stateMu sync.Mutex
	state   string // qr, connecting, connected, offline, loggedout
}

// Emit writes one event line for the window.
func (e *Engine) Emit(v any) {
	b, err := json.Marshal(v)
	if err != nil {
		return
	}
	e.outMu.Lock()
	defer e.outMu.Unlock()
	e.out.Write(b)
	e.out.WriteByte('\n')
	e.out.Flush()
}

func (e *Engine) setState(s string, extra map[string]any) {
	e.stateMu.Lock()
	e.state = s
	e.stateMu.Unlock()
	ev := map[string]any{"t": "state", "state": s}
	for k, v := range extra {
		ev[k] = v
	}
	e.Emit(ev)
}

func (e *Engine) Start() error {
	e.ctx = context.Background()
	// Show up as "Who (Windows)" in the phone's Linked devices.
	store.DeviceProps.Os = proto.String("Who")
	container, err := sqlstore.New(e.ctx, "sqlite",
		"file:"+filepath.Join(e.dataDir, "session.db")+"?_pragma=foreign_keys(1)&_pragma=busy_timeout(5000)&_pragma=journal_mode(WAL)", e.log.Sub("db"))
	if err != nil {
		return fmt.Errorf("session store: %w", err)
	}
	device, err := container.GetFirstDevice(e.ctx)
	if err != nil {
		return fmt.Errorf("session: %w", err)
	}
	e.db, err = OpenStore(filepath.Join(e.dataDir, "messages.db"))
	if err != nil {
		return fmt.Errorf("message store: %w", err)
	}
	e.cli = whatsmeow.NewClient(device, e.log.Sub("client"))
	e.cli.EnableAutoReconnect = true
	e.names = &Names{e: e}
	e.cli.AddEventHandler(e.onEvent)
	go e.connect()
	return nil
}

// connect signs in with the saved session, or shows QR codes for linking.
func (e *Engine) connect() {
	if e.cli.Store.ID == nil {
		qr, err := e.cli.GetQRChannel(e.ctx)
		if err != nil {
			e.setState("offline", map[string]any{"error": err.Error()})
			return
		}
		if err := e.cli.Connect(); err != nil {
			e.setState("offline", map[string]any{"error": err.Error()})
			e.retryLater()
			return
		}
		for item := range qr {
			switch item.Event {
			case "code":
				e.setState("qr", map[string]any{"qr": item.Code, "seconds": int(item.Timeout / time.Second)})
			case "success":
				e.setState("connecting", nil)
			case "timeout":
				e.setState("qrtimeout", nil)
			default:
				if item.Error != nil {
					e.setState("offline", map[string]any{"error": item.Error.Error()})
				}
			}
		}
		return
	}
	e.setState("connecting", nil)
	if err := e.cli.Connect(); err != nil {
		e.setState("offline", map[string]any{"error": err.Error()})
		e.retryLater()
	}
}

func (e *Engine) retryLater() {
	time.AfterFunc(15*time.Second, func() {
		if !e.cli.IsConnected() {
			e.connect()
		}
	})
}

func (e *Engine) Stop() {
	if e.cli != nil {
		e.cli.Disconnect()
	}
}

// fileLog is a whatsmeow logger writing to the engine's log file.
type fileLog struct {
	w      io.Writer
	mod    string
	minLvl int
	mu     *sync.Mutex
}

func newFileLog(w io.Writer, min string) waLog.Logger {
	lv := map[string]int{"DEBUG": 0, "INFO": 1, "WARN": 2, "ERROR": 3}[min]
	return &fileLog{w: w, minLvl: lv, mu: &sync.Mutex{}}
}

func (l *fileLog) out(lv int, name, msg string, args ...any) {
	if lv < l.minLvl {
		return
	}
	l.mu.Lock()
	defer l.mu.Unlock()
	fmt.Fprintf(l.w, "%s [%s %s] %s\n", time.Now().Format("2006-01-02 15:04:05.000"), l.mod, name, fmt.Sprintf(msg, args...))
}

func (l *fileLog) Debugf(msg string, args ...any) { l.out(0, "DEBUG", msg, args...) }
func (l *fileLog) Infof(msg string, args ...any)  { l.out(1, "INFO", msg, args...) }
func (l *fileLog) Warnf(msg string, args ...any)  { l.out(2, "WARN", msg, args...) }
func (l *fileLog) Errorf(msg string, args ...any) { l.out(3, "ERROR", msg, args...) }
func (l *fileLog) Sub(mod string) waLog.Logger {
	m := mod
	if l.mod != "" {
		m = l.mod + "/" + mod
	}
	return &fileLog{w: l.w, mod: m, minLvl: l.minLvl, mu: l.mu}
}
