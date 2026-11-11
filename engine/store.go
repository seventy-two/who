package main

import (
	"database/sql"
	"strings"
	"time"

	_ "modernc.org/sqlite"
)

// Store keeps chats and messages: whatsmeow keeps only keys and contacts,
// so Who has its own history to show.
type Store struct {
	db *sql.DB
}

// Chat is a conversation as the window lists it.
type Chat struct {
	JID        string `json:"jid"`
	Name       string `json:"name"`
	Group      bool   `json:"group"`
	Unread     int    `json:"unread"`
	LastTS     int64  `json:"lastTs"` // Unix ms
	LastText   string `json:"lastText"`
	LastSender string `json:"lastSender,omitempty"`
	LastFromMe bool   `json:"lastFromMe,omitempty"`
	Pinned     bool   `json:"pinned"`
	MutedUntil int64  `json:"mutedUntil"` // Unix ms; -1 for always
	Archived   bool   `json:"archived"`
}

// Msg is one message as the window shows it.
type Msg struct {
	Chat       string              `json:"chat"`
	ID         string              `json:"id"`
	Sender     string              `json:"sender"`
	SenderName string              `json:"senderName,omitempty"`
	FromMe     bool                `json:"fromMe"`
	TS         int64               `json:"ts"` // Unix ms
	Kind       string              `json:"kind"`
	Text       string              `json:"text,omitempty"`
	FileName   string              `json:"fileName,omitempty"`
	Mime       string              `json:"mime,omitempty"`
	Seconds    int                 `json:"seconds,omitempty"`
	Width      int                 `json:"width,omitempty"`
	Height     int                 `json:"height,omitempty"`
	Size       int64               `json:"size,omitempty"`
	Thumb      []byte              `json:"thumb,omitempty"` // a small JPEG, when the message carries one
	ReplyID    string              `json:"replyId,omitempty"`
	ReplyName  string              `json:"replyName,omitempty"`
	ReplyText  string              `json:"replyText,omitempty"`
	Status     string              `json:"status,omitempty"` // sent, delivered, read (own messages)
	Edited     bool                `json:"edited,omitempty"`
	Reactions  map[string][]string `json:"reactions,omitempty"` // emoji -> who
	Media      string              `json:"media,omitempty"`     // local file once downloaded
}

func OpenStore(path string) (*Store, error) {
	db, err := sql.Open("sqlite", "file:"+path+"?_pragma=journal_mode(WAL)&_pragma=busy_timeout(5000)&_pragma=foreign_keys(1)")
	if err != nil {
		return nil, err
	}
	db.SetMaxOpenConns(1)
	_, err = db.Exec(`
CREATE TABLE IF NOT EXISTS chats (
	jid TEXT PRIMARY KEY, name TEXT NOT NULL DEFAULT '', grp INTEGER NOT NULL DEFAULT 0,
	unread INTEGER NOT NULL DEFAULT 0, last_ts INTEGER NOT NULL DEFAULT 0,
	pinned INTEGER NOT NULL DEFAULT 0, muted_until INTEGER NOT NULL DEFAULT 0, archived INTEGER NOT NULL DEFAULT 0
);
CREATE TABLE IF NOT EXISTS messages (
	chat TEXT NOT NULL, id TEXT NOT NULL, sender TEXT NOT NULL, from_me INTEGER NOT NULL, ts INTEGER NOT NULL,
	kind TEXT NOT NULL, text TEXT NOT NULL DEFAULT '', status TEXT NOT NULL DEFAULT '',
	info TEXT NOT NULL DEFAULT '{}', raw BLOB, media TEXT NOT NULL DEFAULT '',
	PRIMARY KEY (chat, id)
);
CREATE INDEX IF NOT EXISTS messages_by_time ON messages (chat, ts);
CREATE TABLE IF NOT EXISTS reactions (
	chat TEXT NOT NULL, id TEXT NOT NULL, sender TEXT NOT NULL, emoji TEXT NOT NULL,
	PRIMARY KEY (chat, id, sender)
);`)
	if err != nil {
		db.Close()
		return nil, err
	}
	return &Store{db: db}, nil
}

func b2i(b bool) int {
	if b {
		return 1
	}
	return 0
}

// UpsertChat makes sure a chat exists; a non-empty name replaces the old.
func (s *Store) UpsertChat(jid, name string, group bool) error {
	_, err := s.db.Exec(`INSERT INTO chats (jid, name, grp) VALUES (?, ?, ?)
		ON CONFLICT (jid) DO UPDATE SET name = CASE WHEN excluded.name != '' THEN excluded.name ELSE chats.name END,
		grp = excluded.grp`, jid, name, b2i(group))
	return err
}

func (s *Store) SetChatState(jid string, unread int, pinned bool, mutedUntil int64, archived bool) error {
	_, err := s.db.Exec(`UPDATE chats SET unread = ?, pinned = ?, muted_until = ?, archived = ? WHERE jid = ?`,
		unread, b2i(pinned), mutedUntil, b2i(archived), jid)
	return err
}

func (s *Store) SetPinned(jid string, v bool) error {
	_, err := s.db.Exec(`UPDATE chats SET pinned = ? WHERE jid = ?`, b2i(v), jid)
	return err
}

func (s *Store) SetMuted(jid string, until int64) error {
	_, err := s.db.Exec(`UPDATE chats SET muted_until = ? WHERE jid = ?`, until, jid)
	return err
}

func (s *Store) SetArchived(jid string, v bool) error {
	_, err := s.db.Exec(`UPDATE chats SET archived = ? WHERE jid = ?`, b2i(v), jid)
	return err
}

func (s *Store) SetUnread(jid string, n int) error {
	_, err := s.db.Exec(`UPDATE chats SET unread = ? WHERE jid = ?`, n, jid)
	return err
}

func (s *Store) AddUnread(jid string) error {
	_, err := s.db.Exec(`UPDATE chats SET unread = unread + 1 WHERE jid = ?`, jid)
	return err
}

// PutMessage stores a message (replacing one with the same id, e.g. an
// edit) and moves the chat's last-message time on. It reports whether the
// message was new.
func (s *Store) PutMessage(m *Msg, raw []byte, info string) (bool, error) {
	var exists int
	s.db.QueryRow(`SELECT COUNT(*) FROM messages WHERE chat = ? AND id = ?`, m.Chat, m.ID).Scan(&exists)
	media := ""
	if exists > 0 {
		s.db.QueryRow(`SELECT media FROM messages WHERE chat = ? AND id = ?`, m.Chat, m.ID).Scan(&media)
	}
	_, err := s.db.Exec(`INSERT INTO messages (chat, id, sender, from_me, ts, kind, text, status, info, raw, media)
		VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
		ON CONFLICT (chat, id) DO UPDATE SET kind = excluded.kind, text = excluded.text, info = excluded.info,
		raw = COALESCE(excluded.raw, messages.raw),
		status = CASE WHEN excluded.status != '' THEN excluded.status ELSE messages.status END`,
		m.Chat, m.ID, m.Sender, b2i(m.FromMe), m.TS, m.Kind, m.Text, m.Status, info, raw, media)
	if err != nil {
		return false, err
	}
	_, err = s.db.Exec(`UPDATE chats SET last_ts = MAX(last_ts, ?) WHERE jid = ?`, m.TS, m.Chat)
	return exists == 0, err
}

// SetStatus moves an own message's status forward (never back).
func (s *Store) SetStatus(chat, id, status string) (bool, error) {
	rank := map[string]int{"": 0, "sent": 1, "delivered": 2, "read": 3, "played": 4}
	var cur string
	if err := s.db.QueryRow(`SELECT status FROM messages WHERE chat = ? AND id = ?`, chat, id).Scan(&cur); err != nil {
		return false, nil
	}
	if rank[status] <= rank[cur] {
		return false, nil
	}
	_, err := s.db.Exec(`UPDATE messages SET status = ? WHERE chat = ? AND id = ?`, status, chat, id)
	return err == nil, err
}

func (s *Store) SetMedia(chat, id, path string) error {
	_, err := s.db.Exec(`UPDATE messages SET media = ? WHERE chat = ? AND id = ?`, path, chat, id)
	return err
}

func (s *Store) Raw(chat, id string) ([]byte, string, error) {
	var raw []byte
	var media string
	err := s.db.QueryRow(`SELECT raw, media FROM messages WHERE chat = ? AND id = ?`, chat, id).Scan(&raw, &media)
	return raw, media, err
}

func (s *Store) Delete(chat, id string) error {
	_, err := s.db.Exec(`UPDATE messages SET kind = 'deleted', text = '', raw = NULL WHERE chat = ? AND id = ?`, chat, id)
	return err
}

func (s *Store) React(chat, id, sender, emoji string) error {
	if emoji == "" {
		_, err := s.db.Exec(`DELETE FROM reactions WHERE chat = ? AND id = ? AND sender = ?`, chat, id, sender)
		return err
	}
	_, err := s.db.Exec(`INSERT INTO reactions (chat, id, sender, emoji) VALUES (?, ?, ?, ?)
		ON CONFLICT (chat, id, sender) DO UPDATE SET emoji = excluded.emoji`, chat, id, sender, emoji)
	return err
}

// Chats lists every chat with its latest message.
func (s *Store) Chats() ([]Chat, error) {
	rows, err := s.db.Query(`SELECT c.jid, c.name, c.grp, c.unread, c.last_ts, c.pinned, c.muted_until, c.archived,
		COALESCE(m.kind, ''), COALESCE(m.text, ''), COALESCE(m.sender, ''), COALESCE(m.from_me, 0)
		FROM chats c LEFT JOIN messages m ON m.rowid = (
			SELECT rowid FROM messages WHERE chat = c.jid ORDER BY ts DESC LIMIT 1)
		ORDER BY c.last_ts DESC`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []Chat
	for rows.Next() {
		var c Chat
		var grp, pinned, archived, fromMe int
		var kind, text, sender string
		if err := rows.Scan(&c.JID, &c.Name, &grp, &c.Unread, &c.LastTS, &pinned, &c.MutedUntil, &archived,
			&kind, &text, &sender, &fromMe); err != nil {
			return nil, err
		}
		c.Group, c.Pinned, c.Archived, c.LastFromMe = grp == 1, pinned == 1, archived == 1, fromMe == 1
		c.LastText = Preview(kind, text)
		c.LastSender = sender
		out = append(out, c)
	}
	return out, rows.Err()
}

// Messages returns up to limit messages before the given time (0 = now),
// oldest first.
func (s *Store) Messages(chat string, before int64, limit int) ([]Msg, error) {
	if before <= 0 {
		before = time.Now().Add(24*time.Hour).UnixMilli()
	}
	rows, err := s.db.Query(`SELECT id, sender, from_me, ts, kind, text, status, info, media FROM messages
		WHERE chat = ? AND ts < ? ORDER BY ts DESC LIMIT ?`, chat, before, limit)
	if err != nil {
		return nil, err
	}
	var out []Msg
	for rows.Next() {
		var m Msg
		var fromMe int
		var info string
		if err := rows.Scan(&m.ID, &m.Sender, &fromMe, &m.TS, &m.Kind, &m.Text, &m.Status, &info, &m.Media); err != nil {
			rows.Close()
			return nil, err
		}
		m.Chat, m.FromMe = chat, fromMe == 1
		applyInfo(&m, info)
		out = append(out, m)
	}
	rows.Close()
	for i, j := 0, len(out)-1; i < j; i, j = i+1, j-1 {
		out[i], out[j] = out[j], out[i]
	}
	for i := range out {
		s.loadReactions(&out[i])
	}
	return out, nil
}

// Message returns one stored message.
func (s *Store) Message(chat, id string) (Msg, error) {
	var m Msg
	var fromMe int
	var info string
	err := s.db.QueryRow(`SELECT id, sender, from_me, ts, kind, text, status, info, media FROM messages WHERE chat = ? AND id = ?`,
		chat, id).Scan(&m.ID, &m.Sender, &fromMe, &m.TS, &m.Kind, &m.Text, &m.Status, &info, &m.Media)
	if err != nil {
		return m, err
	}
	m.Chat, m.FromMe = chat, fromMe == 1
	applyInfo(&m, info)
	s.loadReactions(&m)
	return m, nil
}

func (s *Store) loadReactions(m *Msg) {
	rows, err := s.db.Query(`SELECT sender, emoji FROM reactions WHERE chat = ? AND id = ?`, m.Chat, m.ID)
	if err != nil {
		return
	}
	defer rows.Close()
	for rows.Next() {
		var sender, emoji string
		if rows.Scan(&sender, &emoji) == nil {
			if m.Reactions == nil {
				m.Reactions = map[string][]string{}
			}
			m.Reactions[emoji] = append(m.Reactions[emoji], sender)
		}
	}
}

// Oldest is the earliest stored message in a chat, for asking the phone
// for more history.
func (s *Store) Oldest(chat string) (id, sender string, fromMe bool, ts int64, ok bool) {
	var fm int
	err := s.db.QueryRow(`SELECT id, sender, from_me, ts FROM messages WHERE chat = ? ORDER BY ts ASC LIMIT 1`, chat).
		Scan(&id, &sender, &fm, &ts)
	return id, sender, fm == 1, ts, err == nil
}

// Preview is a chat list's one-line summary of a message.
func Preview(kind, text string) string {
	text = strings.Join(strings.Fields(text), " ")
	label := map[string]string{
		"image": "Photo", "video": "Video", "gif": "GIF", "sticker": "Sticker", "voice": "Voice message",
		"audio": "Audio", "document": "Document", "location": "Location", "contact": "Contact",
		"poll": "Poll", "deleted": "This message was deleted", "unsupported": "Message",
	}[kind]
	switch {
	case label != "" && text != "":
		return label + ": " + text
	case label != "":
		return label
	}
	return text
}
