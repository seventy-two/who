package main

// The window's commands, one JSON object a line on stdin, each with "t":
//
//	chats                         -> {"t":"chats","chats":[...]}
//	messages chat before limit    -> {"t":"messages","chat",...,"messages":[...],"more":bool}
//	send chat text [reply]        -> {"t":"message",...} once sent, or {"t":"error"}
//	sendfile chat path [caption]  -> as send
//	react chat id emoji           -> {"t":"message"} with the reaction
//	read chat                     -> marks the chat read (and tells the phone)
//	media req chat id             -> {"t":"media","req","path"|"error"}
//	typing chat on                -> sends composing/paused
//	older chat                    -> asks the phone for older history
//	logout
//
// Events not asked for: state, chats, chat, message, receipt, typing,
// notify, history, names, error, fatal.

import (
	"encoding/json"
	"fmt"
	"mime"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"time"

	"go.mau.fi/whatsmeow"
	"go.mau.fi/whatsmeow/proto/waE2E"
	"go.mau.fi/whatsmeow/types"
	"go.mau.fi/whatsmeow/types/events"
	"google.golang.org/protobuf/proto"
)

func str(cmd map[string]any, k string) string {
	s, _ := cmd[k].(string)
	return s
}

func num(cmd map[string]any, k string) int64 {
	f, _ := cmd[k].(float64)
	return int64(f)
}

func (e *Engine) Handle(cmd map[string]any) {
	defer func() {
		if r := recover(); r != nil {
			e.log.Errorf("command %v panicked: %v", cmd["t"], r)
			e.Emit(map[string]any{"t": "error", "error": fmt.Sprint(r)})
		}
	}()
	switch str(cmd, "t") {
	case "chats":
		e.emitChats()
	case "messages":
		chat := str(cmd, "chat")
		limit := int(num(cmd, "limit"))
		if limit <= 0 || limit > 500 {
			limit = 80
		}
		ms, err := e.db.Messages(chat, num(cmd, "before"), limit)
		if err != nil {
			e.Emit(map[string]any{"t": "error", "error": err.Error()})
			return
		}
		for i := range ms {
			e.fillMessage(&ms[i])
		}
		if ms == nil {
			ms = []Msg{}
		}
		e.Emit(map[string]any{"t": "messages", "chat": chat, "before": num(cmd, "before"), "messages": ms, "more": len(ms) == limit})
	case "send":
		e.send(cmd)
	case "sendfile":
		e.sendFile(cmd)
	case "react":
		e.react(cmd)
	case "read":
		e.markRead(str(cmd, "chat"))
	case "media":
		e.media(cmd)
	case "typing":
		jid, err := types.ParseJID(str(cmd, "chat"))
		if err == nil {
			state := types.ChatPresencePaused
			if b, _ := cmd["on"].(bool); b {
				state = types.ChatPresenceComposing
			}
			e.cli.SendChatPresence(e.ctx, jid, state, types.ChatPresenceMediaText)
		}
	case "older":
		e.older(str(cmd, "chat"))
	case "online":
		// Typing notifications only arrive while marked available.
		if b, _ := cmd["on"].(bool); b {
			e.cli.SendPresence(e.ctx, types.PresenceAvailable)
		} else {
			e.cli.SendPresence(e.ctx, types.PresenceUnavailable)
		}
	case "logout":
		if err := e.cli.Logout(e.ctx); err != nil {
			e.Emit(map[string]any{"t": "error", "error": err.Error()})
		}
		e.setState("loggedout", nil)
	}
}

func (e *Engine) fail(chat, what string, err error) {
	e.log.Warnf("%s in %s: %v", what, chat, err)
	e.Emit(map[string]any{"t": "error", "chat": chat, "error": what + ": " + err.Error()})
}

// replyContext quotes the message being replied to.
func (e *Engine) replyContext(chat, replyID string) *waE2E.ContextInfo {
	if replyID == "" {
		return nil
	}
	m, err := e.db.Message(chat, replyID)
	if err != nil {
		return nil
	}
	quoted := &waE2E.Message{Conversation: proto.String(Preview(m.Kind, m.Text))}
	if raw, _, _ := e.db.Raw(chat, replyID); len(raw) > 0 {
		var full waE2E.Message
		if proto.Unmarshal(raw, &full) == nil {
			quoted = &full
		}
	}
	return &waE2E.ContextInfo{StanzaID: proto.String(replyID), Participant: proto.String(m.Sender), QuotedMessage: quoted}
}

// sent stores and shows a message once WhatsApp has taken it.
func (e *Engine) sent(chat types.JID, msg *waE2E.Message, resp whatsmeow.SendResponse) {
	evt := &events.Message{Message: msg}
	evt.Info.Chat = chat
	evt.Info.Sender = e.cli.Store.ID.ToNonAD()
	evt.Info.IsFromMe = true
	evt.Info.IsGroup = chat.Server == types.GroupServer
	evt.Info.ID = resp.ID
	evt.Info.Timestamp = resp.Timestamp
	e.onMessage(evt, false)
}

func (e *Engine) send(cmd map[string]any) {
	chatStr := str(cmd, "chat")
	chat, err := types.ParseJID(chatStr)
	if err != nil {
		e.fail(chatStr, "send", err)
		return
	}
	text := str(cmd, "text")
	var msg *waE2E.Message
	if ctx := e.replyContext(chatStr, str(cmd, "reply")); ctx != nil {
		msg = &waE2E.Message{ExtendedTextMessage: &waE2E.ExtendedTextMessage{Text: proto.String(text), ContextInfo: ctx}}
	} else {
		msg = &waE2E.Message{Conversation: proto.String(text)}
	}
	resp, err := e.cli.SendMessage(e.ctx, chat, msg)
	if err != nil {
		e.fail(chatStr, "send", err)
		return
	}
	e.sent(chat, msg, resp)
}

func (e *Engine) sendFile(cmd map[string]any) {
	chatStr := str(cmd, "chat")
	chat, err := types.ParseJID(chatStr)
	if err != nil {
		e.fail(chatStr, "send", err)
		return
	}
	path := str(cmd, "path")
	data, err := os.ReadFile(path)
	if err != nil {
		e.fail(chatStr, "send", err)
		return
	}
	if len(data) > 100<<20 {
		e.fail(chatStr, "send", fmt.Errorf("files over 100 MB can't be sent"))
		return
	}
	caption := str(cmd, "caption")
	mt := mime.TypeByExtension(strings.ToLower(filepath.Ext(path)))
	if mt == "" {
		mt = http.DetectContentType(data)
	}
	ctx := e.replyContext(chatStr, str(cmd, "reply"))
	var msg *waE2E.Message
	kind := whatsmeow.MediaDocument
	if strings.HasPrefix(mt, "image/") && (mt == "image/jpeg" || mt == "image/png" || mt == "image/webp") {
		kind = whatsmeow.MediaImage
	}
	up, err := e.cli.Upload(e.ctx, data, kind)
	if err != nil {
		e.fail(chatStr, "upload", err)
		return
	}
	if kind == whatsmeow.MediaImage {
		w, h, thumb := imageThumb(data)
		msg = &waE2E.Message{ImageMessage: &waE2E.ImageMessage{
			Caption: proto.String(caption), Mimetype: proto.String(mt),
			URL: &up.URL, DirectPath: &up.DirectPath, MediaKey: up.MediaKey,
			FileEncSHA256: up.FileEncSHA256, FileSHA256: up.FileSHA256, FileLength: proto.Uint64(uint64(len(data))),
			Width: proto.Uint32(uint32(w)), Height: proto.Uint32(uint32(h)), JPEGThumbnail: thumb, ContextInfo: ctx,
		}}
	} else {
		msg = &waE2E.Message{DocumentMessage: &waE2E.DocumentMessage{
			Caption: proto.String(caption), Mimetype: proto.String(mt), FileName: proto.String(filepath.Base(path)),
			Title: proto.String(filepath.Base(path)),
			URL:   &up.URL, DirectPath: &up.DirectPath, MediaKey: up.MediaKey,
			FileEncSHA256: up.FileEncSHA256, FileSHA256: up.FileSHA256, FileLength: proto.Uint64(uint64(len(data))),
			ContextInfo: ctx,
		}}
	}
	resp, err := e.cli.SendMessage(e.ctx, chat, msg)
	if err != nil {
		e.fail(chatStr, "send", err)
		return
	}
	e.sent(chat, msg, resp)
	// The sent file is already here: no need to download it again.
	if m, err := e.db.Message(chatStr, resp.ID); err == nil {
		dest := filepath.Join(e.dataDir, "media", safeName(resp.ID)+filepath.Ext(path))
		if os.WriteFile(dest, data, 0o600) == nil {
			e.db.SetMedia(chatStr, m.ID, dest)
			e.emitMessage(chatStr, m.ID)
		}
	}
}

func (e *Engine) react(cmd map[string]any) {
	chatStr, id := str(cmd, "chat"), str(cmd, "id")
	chat, err := types.ParseJID(chatStr)
	if err != nil {
		return
	}
	m, err := e.db.Message(chatStr, id)
	if err != nil {
		e.fail(chatStr, "react", err)
		return
	}
	sender, _ := types.ParseJID(m.Sender)
	emoji := str(cmd, "emoji")
	msg := e.cli.BuildReaction(chat, sender, id, emoji)
	if _, err := e.cli.SendMessage(e.ctx, chat, msg); err != nil {
		e.fail(chatStr, "react", err)
		return
	}
	e.db.React(chatStr, id, e.cli.Store.ID.ToNonAD().String(), emoji)
	e.emitMessage(chatStr, id)
}

// markRead sends read receipts for a chat's unread messages from others.
func (e *Engine) markRead(chatStr string) {
	chat, err := types.ParseJID(chatStr)
	if err != nil {
		return
	}
	var unread int
	for _, c := range e.chatsOnce() {
		if c.JID == chatStr {
			unread = c.Unread
		}
	}
	e.db.SetUnread(chatStr, 0)
	e.emitChat(chatStr)
	if unread == 0 {
		return
	}
	ms, _ := e.db.Messages(chatStr, 0, max(unread, 1)+5)
	bySender := map[string][]types.MessageID{}
	for _, m := range ms {
		if !m.FromMe {
			bySender[m.Sender] = append(bySender[m.Sender], m.ID)
		}
	}
	for s, ids := range bySender {
		sender, _ := types.ParseJID(s)
		if chat.Server != types.GroupServer {
			sender = types.EmptyJID
		}
		e.cli.MarkRead(e.ctx, ids, time.Now(), chat, sender)
	}
}

// media downloads a message's attachment (once) and reports where it is.
func (e *Engine) media(cmd map[string]any) {
	req, chat, id := cmd["req"], str(cmd, "chat"), str(cmd, "id")
	reply := func(path string, err error) {
		ev := map[string]any{"t": "media", "req": req, "chat": chat, "id": id}
		if err != nil {
			ev["error"] = err.Error()
		} else {
			ev["path"] = path
		}
		e.Emit(ev)
	}
	raw, have, err := e.db.Raw(chat, id)
	if err != nil {
		reply("", err)
		return
	}
	if have != "" {
		if _, err := os.Stat(have); err == nil {
			reply(have, nil)
			return
		}
	}
	var msg waE2E.Message
	if len(raw) == 0 || proto.Unmarshal(raw, &msg) != nil {
		reply("", fmt.Errorf("no attachment"))
		return
	}
	data, err := e.cli.DownloadAny(e.ctx, &msg)
	if err != nil {
		reply("", fmt.Errorf("couldn't download it (WhatsApp keeps media for a limited time): %w", err))
		return
	}
	m, _ := e.db.Message(chat, id)
	ext := extFor(m.Mime, m.FileName)
	dest := filepath.Join(e.dataDir, "media", safeName(id)+ext)
	if err := os.WriteFile(dest, data, 0o600); err != nil {
		reply("", err)
		return
	}
	e.db.SetMedia(chat, id, dest)
	reply(dest, nil)
}

// older asks the phone for history before the oldest stored message; it
// arrives as a history sync.
func (e *Engine) older(chatStr string) {
	chat, err := types.ParseJID(chatStr)
	if err != nil {
		return
	}
	id, sender, fromMe, ts, ok := e.db.Oldest(chatStr)
	if !ok {
		return
	}
	senderJ, _ := types.ParseJID(sender)
	info := &types.MessageInfo{
		MessageSource: types.MessageSource{Chat: chat, Sender: senderJ, IsFromMe: fromMe, IsGroup: chat.Server == types.GroupServer},
		ID:            id,
		Timestamp:     time.UnixMilli(ts),
	}
	msg := e.cli.BuildHistorySyncRequest(info, 50)
	own := e.cli.Store.ID.ToNonAD()
	if _, err := e.cli.SendMessage(e.ctx, own, msg, whatsmeow.SendRequestExtra{Peer: true}); err != nil {
		e.fail(chatStr, "history", err)
	}
}

func safeName(s string) string {
	return strings.Map(func(r rune) rune {
		if r >= 'a' && r <= 'z' || r >= 'A' && r <= 'Z' || r >= '0' && r <= '9' || r == '-' || r == '_' {
			return r
		}
		return '_'
	}, s)
}

func extFor(mt, fileName string) string {
	if fileName != "" {
		if ext := filepath.Ext(fileName); ext != "" && len(ext) <= 8 {
			return ext
		}
	}
	switch {
	case strings.HasPrefix(mt, "audio/ogg"):
		return ".ogg"
	case strings.HasPrefix(mt, "image/jpeg"):
		return ".jpg"
	case strings.HasPrefix(mt, "image/png"):
		return ".png"
	case strings.HasPrefix(mt, "image/webp"):
		return ".webp"
	case strings.HasPrefix(mt, "video/mp4"):
		return ".mp4"
	case strings.HasPrefix(mt, "audio/mp4"), strings.HasPrefix(mt, "audio/aac"):
		return ".m4a"
	case strings.HasPrefix(mt, "audio/mpeg"):
		return ".mp3"
	case strings.HasPrefix(mt, "application/pdf"):
		return ".pdf"
	}
	if exts, _ := mime.ExtensionsByType(strings.Split(mt, ";")[0]); len(exts) > 0 {
		return exts[0]
	}
	return ".bin"
}

// extrasOf rebuilds a stored message's extras, for re-saving it.
func extrasOf(m Msg) extras {
	return extras{FileName: m.FileName, Mime: m.Mime, Seconds: m.Seconds, Width: m.Width, Height: m.Height,
		Size: m.Size, Thumb: m.Thumb, ReplyID: m.ReplyID, ReplyFrom: m.ReplyName, ReplyText: m.ReplyText, Edited: m.Edited}
}

func (x extras) json() string {
	b, _ := json.Marshal(x)
	return string(b)
}
