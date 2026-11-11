package main

import (
	"strings"
	"sync"
	"time"

	"go.mau.fi/whatsmeow/types"
	"go.mau.fi/whatsmeow/types/events"
)

// Names turns JIDs into the names to show: groups by subject, people by
// the name saved on the phone, else their own (push) name, else number.
type Names struct {
	e      *Engine
	mu     sync.Mutex
	groups map[string]string
}

func (n *Names) setGroup(jid, name string) {
	n.mu.Lock()
	if n.groups == nil {
		n.groups = map[string]string{}
	}
	n.groups[jid] = name
	n.mu.Unlock()
}

// Of names a user or group JID.
// isMe says whether jid is this account, by phone number or by LID (groups
// quote your own messages by LID).
func (n *Names) isMe(jid types.JID) bool {
	st := n.e.cli.Store
	if st.ID != nil && jid.User == st.ID.User {
		return true
	}
	if !st.LID.IsEmpty() && jid.User == st.LID.User {
		return true
	}
	if jid.Server == types.HiddenUserServer && st.ID != nil {
		if pn, err := st.LIDs.GetPNForLID(n.e.ctx, jid); err == nil && pn.User == st.ID.User {
			return true
		}
	}
	return false
}

func (n *Names) Of(jidStr string) string {
	jid, err := types.ParseJID(jidStr)
	if err != nil {
		return jidStr
	}
	if jid.Server == types.GroupServer {
		n.mu.Lock()
		name := n.groups[jid.String()]
		n.mu.Unlock()
		if name != "" {
			return name
		}
		return "Group"
	}
	if n.isMe(jid) {
		return "You"
	}
	ctx := n.e.ctx
	try := []types.JID{jid}
	// Newer chats and group members are by LID; their contact entry is
	// under the phone number.
	if jid.Server == types.HiddenUserServer {
		if pn, err := n.e.cli.Store.LIDs.GetPNForLID(ctx, jid); err == nil && !pn.IsEmpty() {
			try = append(try, pn)
		}
	} else if lid, err := n.e.cli.Store.LIDs.GetLIDForPN(ctx, jid); err == nil && !lid.IsEmpty() {
		try = append(try, lid)
	}
	var push, business, redacted string
	for _, j := range try {
		c, err := n.e.cli.Store.Contacts.GetContact(ctx, j)
		if err != nil || !c.Found {
			continue
		}
		if c.FullName != "" {
			return c.FullName
		}
		if c.FirstName != "" {
			return c.FirstName
		}
		if push == "" {
			push = c.PushName
		}
		if business == "" {
			business = c.BusinessName
		}
		if redacted == "" {
			redacted = c.RedactedPhone
		}
	}
	switch {
	case push != "":
		return "~" + push // like WhatsApp: a name they gave, not one you saved
	case business != "":
		return business
	}
	for _, j := range try {
		if j.Server == types.DefaultUserServer {
			return "+" + j.User
		}
	}
	if redacted != "" {
		return redacted
	}
	return jid.User
}

// chatJID is the JID a chat is kept under. LID chats are moved to the phone
// number when it's known, so one person isn't two chats.
func (e *Engine) chatJID(j types.JID) types.JID {
	j = j.ToNonAD()
	if j.Server == types.HiddenUserServer {
		if pn, err := e.cli.Store.LIDs.GetPNForLID(e.ctx, j); err == nil && !pn.IsEmpty() {
			return pn.ToNonAD()
		}
	}
	return j
}

func (e *Engine) onEvent(evt any) {
	switch v := evt.(type) {
	case *events.Connected:
		e.setState("connected", map[string]any{"me": e.cli.Store.ID.User, "name": e.cli.Store.PushName})
		go e.loadGroups()
	case *events.PairSuccess:
		e.setState("connecting", nil)
	case *events.Disconnected:
		e.setState("offline", nil)
	case *events.StreamReplaced:
		e.setState("offline", map[string]any{"error": "Who was opened somewhere else with the same session"})
	case *events.LoggedOut:
		e.setState("loggedout", map[string]any{"reason": v.Reason.String()})
	case *events.Message:
		e.onMessage(v, false)
	case *events.HistorySync:
		go e.onHistory(v)
	case *events.Receipt:
		status := ""
		switch v.Type {
		case types.ReceiptTypeDelivered:
			status = "delivered"
		case types.ReceiptTypeRead, types.ReceiptTypeReadSelf:
			status = "read"
		case types.ReceiptTypePlayed:
			status = "played"
		}
		if status == "" {
			return
		}
		chat := e.chatJID(v.Chat).String()
		if v.Type == types.ReceiptTypeReadSelf {
			// Read on another of your devices.
			e.db.SetUnread(chat, 0)
			e.emitChat(chat)
			return
		}
		var changed []string
		for _, id := range v.MessageIDs {
			if ok, _ := e.db.SetStatus(chat, id, status); ok {
				changed = append(changed, id)
			}
		}
		if len(changed) > 0 {
			e.Emit(map[string]any{"t": "receipt", "chat": chat, "ids": changed, "status": status})
		}
	case *events.ChatPresence:
		e.Emit(map[string]any{"t": "typing", "chat": e.chatJID(v.Chat).String(), "sender": v.Sender.ToNonAD().String(),
			"name": e.names.Of(v.Sender.ToNonAD().String()),
			"state": string(v.State), "media": string(v.Media)})
	case *events.Pin:
		if v.Action != nil {
			chat := e.chatJID(v.JID).String()
			e.db.SetPinned(chat, v.Action.GetPinned())
			e.emitChat(chat)
		}
	case *events.Mute:
		if v.Action != nil {
			chat := e.chatJID(v.JID).String()
			until := int64(0)
			if v.Action.GetMuted() {
				until = v.Action.GetMuteEndTimestamp()
				if until <= 0 {
					until = -1
				} else if until < 1e12 {
					until *= 1000
				}
			}
			e.db.SetMuted(chat, until)
			e.emitChat(chat)
		}
	case *events.Archive:
		if v.Action != nil {
			chat := e.chatJID(v.JID).String()
			e.db.SetArchived(chat, v.Action.GetArchived())
			e.emitChat(chat)
		}
	case *events.MarkChatAsRead:
		if v.Action != nil {
			chat := e.chatJID(v.JID).String()
			if v.Action.GetRead() {
				e.db.SetUnread(chat, 0)
			} else {
				e.db.SetUnread(chat, 1)
			}
			e.emitChat(chat)
		}
	case *events.GroupInfo:
		if v.Name != nil {
			jid := v.JID.String()
			e.names.setGroup(jid, v.Name.Name)
			e.db.UpsertChat(jid, v.Name.Name, true)
			e.emitChat(jid)
		}
	case *events.JoinedGroup:
		jid := v.JID.String()
		e.names.setGroup(jid, v.GroupInfo.Name)
		e.db.UpsertChat(jid, v.GroupInfo.Name, true)
		e.emitChat(jid)
	case *events.Contact, *events.PushName:
		e.Emit(map[string]any{"t": "names"}) // the window asks again
	case *events.OfflineSyncCompleted:
		e.emitChats()
	}
}

// loadGroups names every group the account is in.
func (e *Engine) loadGroups() {
	groups, err := e.cli.GetJoinedGroups(e.ctx)
	if err != nil {
		e.log.Warnf("groups: %v", err)
		return
	}
	for _, g := range groups {
		jid := g.JID.String()
		e.names.setGroup(jid, g.Name)
		e.db.UpsertChat(jid, g.Name, true)
	}
	e.emitChats()
}

// onMessage stores a message (or applies a reaction, edit or deletion) and
// tells the window. From history, it neither notifies nor counts unread.
func (e *Engine) onMessage(evt *events.Message, history bool) {
	chatJ := e.chatJID(evt.Info.Chat)
	if chatJ.Server == types.BroadcastServer || chatJ.Server == types.NewsletterServer {
		return // status updates and channels: not shown
	}
	c := Convert(evt)
	c.Msg.Chat = chatJ.String()
	chat := c.Msg.Chat
	if c.Skip {
		return
	}
	group := chatJ.Server == types.GroupServer
	e.db.UpsertChat(chat, "", group)
	switch {
	case c.ReactTo != "":
		sender := evt.Info.Sender.ToNonAD().String()
		e.db.React(chat, c.ReactTo, sender, c.ReactEmoji)
		e.emitMessage(chat, c.ReactTo)
		return
	case c.Revokes != "":
		e.db.Delete(chat, c.Revokes)
		e.emitMessage(chat, c.Revokes)
		e.emitChat(chat)
		return
	case c.Edits != "":
		if m, err := e.db.Message(chat, c.Edits); err == nil {
			m.Text = c.Msg.Text
			info := extrasOf(m)
			info.Edited = true
			e.db.PutMessage(&m, nil, info.json())
			e.emitMessage(chat, c.Edits)
		}
		return
	}
	isNew, err := e.db.PutMessage(&c.Msg, c.Raw, c.Info)
	if err != nil {
		e.log.Warnf("store: %v", err)
		return
	}
	if history {
		return
	}
	if isNew && !c.Msg.FromMe {
		e.db.AddUnread(chat)
	}
	if c.Msg.FromMe {
		e.db.SetUnread(chat, 0) // replied from the phone
	}
	m := c.Msg
	m.SenderName = e.names.Of(m.Sender)
	if m.ReplyName != "" {
		m.ReplyName = e.names.Of(m.ReplyName)
	}
	e.Emit(map[string]any{"t": "message", "message": m, "new": isNew})
	e.emitChat(chat)
	if isNew && !c.Msg.FromMe && time.Since(evt.Info.Timestamp) < 10*time.Minute {
		e.Emit(map[string]any{"t": "notify", "chat": chat, "chatName": e.chatName(chat, group),
			"sender": m.SenderName, "text": Preview(m.Kind, m.Text), "group": group, "muted": e.muted(chat)})
	}
}

func (e *Engine) muted(chat string) bool {
	for _, c := range e.chatsOnce() {
		if c.JID == chat {
			return c.MutedUntil == -1 || c.MutedUntil > time.Now().UnixMilli()
		}
	}
	return false
}

func (e *Engine) chatsOnce() []Chat {
	cs, _ := e.db.Chats()
	return cs
}

// onHistory stores the history the phone sends after linking (and later
// on request): chats with their state, and their messages.
func (e *Engine) onHistory(evt *events.HistorySync) {
	data := evt.Data
	for _, p := range data.GetPushnames() {
		_ = p // whatsmeow stores these itself
	}
	n := 0
	for _, conv := range data.GetConversations() {
		jid, err := types.ParseJID(conv.GetID())
		if err != nil {
			continue
		}
		chatJ := e.chatJID(jid)
		if chatJ.Server == types.BroadcastServer || chatJ.Server == types.NewsletterServer {
			continue
		}
		chat := chatJ.String()
		group := chatJ.Server == types.GroupServer
		name := conv.GetName()
		if name == "" {
			name = conv.GetDisplayName()
		}
		if group && name != "" {
			e.names.setGroup(chat, name)
		}
		e.db.UpsertChat(chat, map[bool]string{true: name, false: ""}[group], group)
		for _, hm := range conv.GetMessages() {
			wm := hm.GetMessage()
			if wm == nil {
				continue
			}
			evt, err := e.cli.ParseWebMessage(jid, wm)
			if err != nil {
				continue
			}
			e.onMessage(evt, true)
			// Status of own messages from the history record.
			if evt.Info.IsFromMe {
				switch wm.GetStatus() {
				case 3:
					e.db.SetStatus(chat, evt.Info.ID, "delivered")
				case 4, 5:
					e.db.SetStatus(chat, evt.Info.ID, "read")
				}
			}
			n++
		}
		mute := conv.GetMuteEndTime()
		if mute > 0 && mute < 1e12 {
			mute *= 1000
		}
		if int64(mute) != 0 || conv.GetPinned() > 0 || conv.GetArchived() || conv.GetUnreadCount() > 0 || conv.GetMarkedAsUnread() {
			unread := int(conv.GetUnreadCount())
			if unread == 0 && conv.GetMarkedAsUnread() {
				unread = 1
			}
			e.db.SetChatState(chat, unread, conv.GetPinned() > 0, int64(mute), conv.GetArchived())
		}
	}
	e.log.Infof("history sync (%s): %d chats, %d messages", data.GetSyncType().String(), len(data.GetConversations()), n)
	e.emitChats()
	e.Emit(map[string]any{"t": "history", "messages": n})
}

func (e *Engine) chatName(chat string, group bool) string {
	return e.names.Of(chat)
}

// emitChats sends the whole chat list, names filled in.
func (e *Engine) emitChats() {
	cs, err := e.db.Chats()
	if err != nil {
		return
	}
	if cs == nil {
		cs = []Chat{}
	}
	for i := range cs {
		e.fillChat(&cs[i])
	}
	e.Emit(map[string]any{"t": "chats", "chats": cs})
}

func (e *Engine) emitChat(chat string) {
	for _, c := range e.chatsOnce() {
		if c.JID == chat {
			e.fillChat(&c)
			e.Emit(map[string]any{"t": "chat", "chat": c})
			return
		}
	}
}

func (e *Engine) fillChat(c *Chat) {
	if !c.Group || c.Name == "" {
		c.Name = e.names.Of(c.JID)
	}
	if c.LastSender != "" && c.Group && !c.LastFromMe {
		name := e.names.Of(c.LastSender)
		c.LastSender = strings.TrimPrefix(name, "~")
	} else {
		c.LastSender = ""
	}
}

func (e *Engine) emitMessage(chat, id string) {
	m, err := e.db.Message(chat, id)
	if err != nil {
		return
	}
	e.fillMessage(&m)
	e.Emit(map[string]any{"t": "message", "message": m, "new": false})
}

func (e *Engine) fillMessage(m *Msg) {
	m.SenderName = e.names.Of(m.Sender)
	if m.ReplyName != "" {
		m.ReplyName = e.names.Of(m.ReplyName)
	}
	for emoji, who := range m.Reactions {
		names := make([]string, len(who))
		for i, w := range who {
			names[i] = e.names.Of(w)
		}
		m.Reactions[emoji] = names
	}
}
