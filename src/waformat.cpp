#include "waformat.h"

#include <cctype>

namespace who {

namespace {

bool wordChar(unsigned char c) { return std::isalnum(c) || c >= 0x80; }

// closing finds the end marker for a *bold*-style run starting at i: the
// marker must hug the text (no space just inside) and not sit inside a
// word, as WhatsApp does.
size_t closing(std::string_view s, size_t i, std::string_view mark) {
    size_t start = i + mark.size();
    if (start >= s.size() || s[start] == ' ') return std::string_view::npos;
    if (i > 0 && wordChar((unsigned char)s[i - 1])) return std::string_view::npos;
    size_t j = s.find(mark, start + 1);
    while (j != std::string_view::npos) {
        bool hug = s[j - 1] != ' ';
        bool after = j + mark.size() >= s.size() || !wordChar((unsigned char)s[j + mark.size()]);
        if (hug && after) return j;
        j = s.find(mark, j + 1);
    }
    return std::string_view::npos;
}

bool linkAt(std::string_view s, size_t i, size_t* len) {
    auto starts = [&](const char* p) { return s.compare(i, std::char_traits<char>::length(p), p) == 0; };
    if (!(starts("http://") || starts("https://") || starts("www."))) return false;
    if (i > 0 && wordChar((unsigned char)s[i - 1])) return false;
    size_t j = i;
    while (j < s.size() && !std::isspace((unsigned char)s[j])) j++;
    // Trailing punctuation belongs to the sentence.
    while (j > i && std::string_view(".,;:!?)'\"").find(s[j - 1]) != std::string_view::npos) j--;
    *len = j - i;
    return *len > 4;
}

void spans(std::string_view s, uint16_t style, std::vector<Span>& out) {
    std::string text;
    auto flush = [&] {
        if (text.empty()) return;
        Span sp;
        sp.text = std::move(text);
        sp.style = style;
        out.push_back(std::move(sp));
        text.clear();
    };
    for (size_t i = 0; i < s.size();) {
        // ```monospace```
        if (s.compare(i, 3, "```") == 0) {
            size_t j = s.find("```", i + 3);
            if (j != std::string_view::npos && j > i + 3) {
                flush();
                Span sp;
                sp.text = std::string(s.substr(i + 3, j - i - 3));
                sp.style = style | style::Code;
                out.push_back(std::move(sp));
                i = j + 3;
                continue;
            }
        }
        // `inline code`
        if (s[i] == '`') {
            size_t j = s.find('`', i + 1);
            if (j != std::string_view::npos && j > i + 1) {
                flush();
                Span sp;
                sp.text = std::string(s.substr(i + 1, j - i - 1));
                sp.style = style | style::Code;
                out.push_back(std::move(sp));
                i = j + 1;
                continue;
            }
        }
        static const struct {
            char c;
            uint16_t st;
        } marks[] = {{'*', style::Bold}, {'_', style::Italic}, {'~', style::Strike}};
        bool done = false;
        for (const auto& m : marks) {
            if (s[i] != m.c) continue;
            size_t j = closing(s, i, std::string_view(&m.c, 1));
            if (j == std::string_view::npos) continue;
            flush();
            spans(s.substr(i + 1, j - i - 1), style | m.st, out);
            i = j + 1;
            done = true;
            break;
        }
        if (done) continue;
        size_t len = 0;
        if (linkAt(s, i, &len)) {
            flush();
            Span sp;
            sp.kind = Span::Kind::Link;
            sp.text = std::string(s.substr(i, len));
            sp.url = s.compare(i, 4, "www.") == 0 ? "https://" + sp.text : sp.text;
            sp.style = style;
            out.push_back(std::move(sp));
            i += len;
            continue;
        }
        std::string file;
        if (size_t n = EmojiAt(s, i, &file)) {
            flush();
            Span sp;
            sp.kind = Span::Kind::Emoji;
            sp.text = std::string(s.substr(i, n));
            sp.file = file;
            sp.style = style;
            out.push_back(std::move(sp));
            i += n;
            continue;
        }
        text += s[i++];
    }
    flush();
}

}  // namespace

std::vector<Para> ParseWhatsApp(std::string_view text, bool* jumbo) {
    std::vector<Para> out;
    // A ```block``` over several lines is one code paragraph.
    size_t i = 0;
    while (i <= text.size()) {
        if (text.compare(i, 3, "```") == 0) {
            size_t end = text.find("```", i + 3);
            if (end != std::string_view::npos && text.substr(i + 3, end - i - 3).find('\n') != std::string_view::npos) {
                Para p;
                p.code = true;
                std::string_view body = text.substr(i + 3, end - i - 3);
                if (!body.empty() && body.front() == '\n') body.remove_prefix(1);
                if (!body.empty() && body.back() == '\n') body.remove_suffix(1);
                p.text = std::string(body);
                out.push_back(std::move(p));
                i = end + 3;
                if (i < text.size() && text[i] == '\n') i++;
                if (i >= text.size()) break;
                continue;
            }
        }
        size_t nl = text.find('\n', i);
        if (nl == std::string_view::npos) nl = text.size();
        std::string_view line = text.substr(i, nl - i);
        Para p;
        if (line.size() >= 2 && line[0] == '>' && line[1] == ' ') {
            p.quote = true;
            line.remove_prefix(2);
        } else if (line.size() >= 2 && (line[0] == '-' || line[0] == '*') && line[1] == ' ') {
            p.list = 1;
            p.marker = "\xE2\x80\xA2";
            line.remove_prefix(2);
        }
        spans(line, 0, p.spans);
        out.push_back(std::move(p));
        i = nl + 1;
    }
    if (jumbo) {
        // Only a few emoji and spaces: drawn big, as WhatsApp does.
        int emoji = 0;
        bool other = false;
        for (const auto& p : out)
            for (const auto& sp : p.spans) {
                if (sp.kind == Span::Kind::Emoji) emoji++;
                else if (sp.text.find_first_not_of(" ") != std::string::npos) other = true;
            }
        *jumbo = !other && emoji > 0 && emoji <= 3 && out.size() == 1;
    }
    return out;
}

}  // namespace who
