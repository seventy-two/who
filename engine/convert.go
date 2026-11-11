package main

import (
	"encoding/json"

	"go.mau.fi/whatsmeow/proto/waE2E"
	"go.mau.fi/whatsmeow/types/events"
	"google.golang.org/protobuf/proto"
	"google.golang.org/protobuf/reflect/protoreflect"
)

// extras is the part of a Msg kept as JSON in the store's info column.
type extras struct {
	FileName  string `json:"fileName,omitempty"`
	Mime      string `json:"mime,omitempty"`
	Seconds   int    `json:"seconds,omitempty"`
	Width     int    `json:"width,omitempty"`
	Height    int    `json:"height,omitempty"`
	Size      int64  `json:"size,omitempty"`
	Thumb     []byte `json:"thumb,omitempty"`
	ReplyID   string `json:"replyId,omitempty"`
	ReplyFrom string `json:"replyFrom,omitempty"`
	ReplyText string `json:"replyText,omitempty"`
	Edited    bool   `json:"edited,omitempty"`
}

func applyInfo(m *Msg, info string) {
	var x extras
	if json.Unmarshal([]byte(info), &x) != nil {
		return
	}
	m.FileName, m.Mime, m.Seconds, m.Width, m.Height, m.Size, m.Thumb = x.FileName, x.Mime, x.Seconds, x.Width, x.Height, x.Size, x.Thumb
	m.ReplyID, m.ReplyText, m.Edited = x.ReplyID, x.ReplyText, x.Edited
	m.ReplyName = x.ReplyFrom // the window turns this JID into a name
}

// Converted is a message ready to store, or one of the special kinds that
// change another message (reaction, revoke, edit).
type Converted struct {
	Msg  Msg
	Info string
	Raw  []byte

	// Set instead of a new message when it changes another one.
	ReactTo    string
	ReactEmoji string
	Revokes    string
	Edits      string
	Skip       bool // protocol chatter with nothing to show
}

// Convert reads a WhatsApp message into what the window shows.
func Convert(evt *events.Message) Converted {
	info := evt.Info
	c := Converted{Msg: Msg{
		Chat:   info.Chat.ToNonAD().String(),
		ID:     info.ID,
		Sender: info.Sender.ToNonAD().String(),
		FromMe: info.IsFromMe,
		TS:     info.Timestamp.UnixMilli(),
	}}
	if c.Msg.FromMe {
		c.Msg.Status = "sent"
	}
	m := evt.Message
	if m == nil {
		c.Skip = true
		return c
	}
	var x extras
	ctx := (*waE2E.ContextInfo)(nil)
	setMedia := func(mime string, size uint64, thumb []byte) {
		x.Mime = mime
		x.Size = int64(size)
		if len(thumb) > 0 && len(thumb) < 64<<10 {
			x.Thumb = thumb
		}
	}

	switch {
	case m.GetReactionMessage() != nil:
		r := m.GetReactionMessage()
		c.ReactTo = r.GetKey().GetID()
		c.ReactEmoji = r.GetText()
		return c
	case m.GetProtocolMessage() != nil:
		p := m.GetProtocolMessage()
		switch p.GetType() {
		case waE2E.ProtocolMessage_REVOKE:
			c.Revokes = p.GetKey().GetID()
		case waE2E.ProtocolMessage_MESSAGE_EDIT:
			c.Edits = p.GetKey().GetID()
			if e := p.GetEditedMessage(); e != nil {
				c.Msg.Text = textOf(e)
			}
		default:
			c.Skip = true
		}
		return c
	case m.GetConversation() != "":
		c.Msg.Kind, c.Msg.Text = "text", m.GetConversation()
	case m.GetExtendedTextMessage() != nil:
		t := m.GetExtendedTextMessage()
		c.Msg.Kind, c.Msg.Text = "text", t.GetText()
		ctx = t.GetContextInfo()
	case m.GetImageMessage() != nil:
		i := m.GetImageMessage()
		c.Msg.Kind, c.Msg.Text = "image", i.GetCaption()
		setMedia(i.GetMimetype(), i.GetFileLength(), i.GetJPEGThumbnail())
		x.Width, x.Height = int(i.GetWidth()), int(i.GetHeight())
		ctx = i.GetContextInfo()
	case m.GetStickerMessage() != nil:
		s := m.GetStickerMessage()
		c.Msg.Kind = "sticker"
		setMedia(s.GetMimetype(), s.GetFileLength(), s.GetPngThumbnail())
		x.Width, x.Height = int(s.GetWidth()), int(s.GetHeight())
		ctx = s.GetContextInfo()
	case m.GetVideoMessage() != nil:
		v := m.GetVideoMessage()
		c.Msg.Kind, c.Msg.Text = "video", v.GetCaption()
		if v.GetGifPlayback() {
			c.Msg.Kind = "gif"
		}
		setMedia(v.GetMimetype(), v.GetFileLength(), v.GetJPEGThumbnail())
		x.Width, x.Height, x.Seconds = int(v.GetWidth()), int(v.GetHeight()), int(v.GetSeconds())
		ctx = v.GetContextInfo()
	case m.GetAudioMessage() != nil:
		a := m.GetAudioMessage()
		c.Msg.Kind = "audio"
		if a.GetPTT() {
			c.Msg.Kind = "voice"
		}
		setMedia(a.GetMimetype(), a.GetFileLength(), nil)
		x.Seconds = int(a.GetSeconds())
		ctx = a.GetContextInfo()
	case m.GetDocumentMessage() != nil:
		d := m.GetDocumentMessage()
		c.Msg.Kind, c.Msg.Text = "document", d.GetCaption()
		x.FileName = d.GetFileName()
		if x.FileName == "" {
			x.FileName = d.GetTitle()
		}
		setMedia(d.GetMimetype(), d.GetFileLength(), d.GetJPEGThumbnail())
		ctx = d.GetContextInfo()
	case m.GetLocationMessage() != nil:
		l := m.GetLocationMessage()
		c.Msg.Kind = "location"
		c.Msg.Text = l.GetName()
		if c.Msg.Text == "" {
			c.Msg.Text = l.GetAddress()
		}
		ctx = l.GetContextInfo()
	case m.GetLiveLocationMessage() != nil:
		c.Msg.Kind, c.Msg.Text = "location", "Live location"
	case m.GetContactMessage() != nil:
		c.Msg.Kind, c.Msg.Text = "contact", m.GetContactMessage().GetDisplayName()
	case m.GetContactsArrayMessage() != nil:
		c.Msg.Kind, c.Msg.Text = "contact", m.GetContactsArrayMessage().GetDisplayName()
	case m.GetPollCreationMessage() != nil || m.GetPollCreationMessageV3() != nil:
		p := m.GetPollCreationMessage()
		if p == nil {
			p = m.GetPollCreationMessageV3()
		}
		c.Msg.Kind, c.Msg.Text = "poll", p.GetName()
	case m.GetPollUpdateMessage() != nil, m.GetSenderKeyDistributionMessage() != nil:
		// Votes, and group key handouts with nothing else in them.
		c.Skip = true
		return c
	default:
		// Context info alone is protocol chatter; anything else is a kind
		// Who doesn't draw.
		if countFields(m) <= 1 && m.GetMessageContextInfo() != nil {
			c.Skip = true
			return c
		}
		c.Msg.Kind = "unsupported"
	}
	if ctx != nil && ctx.GetStanzaID() != "" {
		x.ReplyID = ctx.GetStanzaID()
		x.ReplyFrom = ctx.GetParticipant()
		x.ReplyText = Preview(kindOf(ctx.GetQuotedMessage()), textOf(ctx.GetQuotedMessage()))
	}
	x.Edited = evt.IsEdit
	c.Msg.FileName, c.Msg.Mime, c.Msg.Seconds, c.Msg.Width, c.Msg.Height, c.Msg.Size, c.Msg.Thumb =
		x.FileName, x.Mime, x.Seconds, x.Width, x.Height, x.Size, x.Thumb
	c.Msg.ReplyID, c.Msg.ReplyText, c.Msg.ReplyName, c.Msg.Edited = x.ReplyID, x.ReplyText, x.ReplyFrom, x.Edited
	b, _ := json.Marshal(x)
	c.Info = string(b)
	if c.Msg.Kind != "text" {
		c.Raw, _ = proto.Marshal(m) // kept so media can be downloaded later
	}
	return c
}

// countFields counts a message's set fields (to spot ones carrying only
// context info).
func countFields(m *waE2E.Message) int {
	n := 0
	m.ProtoReflect().Range(func(protoreflect.FieldDescriptor, protoreflect.Value) bool { n++; return true })
	return n
}

func textOf(m *waE2E.Message) string {
	switch {
	case m == nil:
		return ""
	case m.GetConversation() != "":
		return m.GetConversation()
	case m.GetExtendedTextMessage() != nil:
		return m.GetExtendedTextMessage().GetText()
	case m.GetImageMessage() != nil:
		return m.GetImageMessage().GetCaption()
	case m.GetVideoMessage() != nil:
		return m.GetVideoMessage().GetCaption()
	case m.GetDocumentMessage() != nil:
		return m.GetDocumentMessage().GetFileName()
	}
	return ""
}

func kindOf(m *waE2E.Message) string {
	switch {
	case m == nil:
		return ""
	case m.GetImageMessage() != nil:
		return "image"
	case m.GetStickerMessage() != nil:
		return "sticker"
	case m.GetVideoMessage() != nil:
		return "video"
	case m.GetAudioMessage() != nil:
		if m.GetAudioMessage().GetPTT() {
			return "voice"
		}
		return "audio"
	case m.GetDocumentMessage() != nil:
		return "document"
	}
	return "text"
}
