// Discord's message markdown, parsed into styled runs for drawing, and
// the Unicode emoji found in text. Portable C++.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace who {

namespace style {
constexpr uint16_t Bold = 1, Italic = 2, Underline = 4, Strike = 8, Code = 16, Spoiler = 32;
}

struct Span {
    enum class Kind {
        Text,
        Emoji,           // text is the emoji, file its Twemoji name ("1f600")
        CustomEmoji,     // id, text its name, animated
        UserMention,     // id
        RoleMention,     // id
        ChannelMention,  // id
        Everyone,        // text is "@everyone" or "@here"
        Command,         // a slash command, text "/name"
        Link,            // text and url
        Timestamp,       // time (Unix seconds) and format
    } kind = Kind::Text;
    std::string text, url, file;
    uint64_t id = 0;
    bool animated = false;
    int64_t time = 0;
    char format = 'f';
    uint16_t style = 0;
    int spoiler = 0;  // which spoiler, counting from 1, so each reveals on its own
};

// Para is a line of a message, or a code block.
struct Para {
    bool code = false;  // a ``` block: text and lang
    std::string text, lang;
    bool quote = false;
    int heading = 0;  // 1 to 3 for #, ##, ###
    bool subtext = false;  // -#
    int list = 0;         // indent level + 1 for a bullet or numbered item
    std::string marker;   // "•" or "3."
    std::vector<Span> spans;
};

// AnsiRun is a stretch of an ```ansi block's text, [start, end) in bytes
// of the text with its escapes taken out, and the SGR style it has: fg
// 30-37 and bg 40-47 (0 for none), as Discord draws them.
struct AnsiRun {
    size_t start = 0, end = 0;
    int fg = 0, bg = 0;
    bool bold = false, underline = false;
};

// StripAnsi takes the ESC[...m escapes out of an ```ansi block, giving the
// text to show and its styled runs. Styles carry over line breaks; codes
// Discord ignores (dim, italic, 256 colours…) are dropped.
std::string StripAnsi(std::string_view s, std::vector<AnsiRun>* runs);

// Parse reads a message's content. Messages with nothing but up to 30
// emoji (and spaces) are drawn large, as Discord does: jumbo says so.
std::vector<Para> Parse(std::string_view content, bool* jumbo = nullptr);

// EmojiAt measures the Unicode emoji sequence starting at s[i]: its
// length in bytes, or 0 if there isn't one. file gets its Twemoji name.
size_t EmojiAt(std::string_view s, size_t i, std::string* file = nullptr);

// TwemojiName turns an emoji into Twemoji's file name: its code points in
// hex joined by '-', without U+FE0F unless it's a ZWJ sequence.
std::string TwemojiName(std::string_view emoji);

// EmojiEntry is one emoji of the picker and shortcode table.
struct EmojiEntry {
    const char* emoji;
    const char* names;  // space-separated shortcodes, the first the main one
    int category;
};
const std::vector<EmojiEntry>& EmojiTable();
extern const char* const kEmojiCategories[9];
// EmojiByName finds an emoji by shortcode ("thumbsup"); nullptr if none.
const EmojiEntry* EmojiByName(std::string_view name);
// EmojiByChar finds an emoji's entry from the emoji itself, with or without
// U+FE0F; nullptr if it isn't in the table.
const EmojiEntry* EmojiByChar(std::string_view emoji);
// FirstName is an entry's main shortcode.
std::string FirstName(const EmojiEntry& e);

// DecodeUtf8 reads one code point at s[i], advancing i; U+FFFD for bad bytes.
uint32_t DecodeUtf8(std::string_view s, size_t& i);
void AppendUtf8(std::string& out, uint32_t cp);

}  // namespace who
