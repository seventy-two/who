#include "markdown.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

namespace who {

const char* const kEmojiCategories[9] = {"Smileys & emotion", "People & body", "Animals & nature",
                                         "Food & drink",      "Travel & places", "Activities",
                                         "Objects",           "Symbols",        "Flags"};

const std::vector<EmojiEntry>& EmojiTable() {
    static const std::vector<EmojiEntry> table = {
#include "emoji_data.inc"
    };
    return table;
}

std::string FirstName(const EmojiEntry& e) {
    const char* sp = strchr(e.names, ' ');
    return sp ? std::string(e.names, sp) : std::string(e.names);
}

const EmojiEntry* EmojiByName(std::string_view name) {
    static const auto index = [] {
        std::unordered_map<std::string, const EmojiEntry*> m;
        for (const auto& e : EmojiTable()) {
            std::string names = e.names;
            size_t start = 0;
            while (start < names.size()) {
                size_t end = names.find(' ', start);
                if (end == std::string::npos) end = names.size();
                m.emplace(names.substr(start, end - start), &e);
                start = end + 1;
            }
        }
        return m;
    }();
    auto it = index.find(std::string(name));
    return it == index.end() ? nullptr : it->second;
}

const EmojiEntry* EmojiByChar(std::string_view emoji) {
    static const auto index = [] {
        std::unordered_map<std::string, const EmojiEntry*> m;
        for (const auto& e : EmojiTable()) m.emplace(TwemojiName(e.emoji), &e);
        return m;
    }();
    auto it = index.find(TwemojiName(emoji));
    return it == index.end() ? nullptr : it->second;
}

uint32_t DecodeUtf8(std::string_view s, size_t& i) {
    unsigned char c = (unsigned char)s[i];
    auto cont = [&](size_t k) { return i + k < s.size() && ((unsigned char)s[i + k] & 0xC0) == 0x80; };
    uint32_t cp;
    if (c < 0x80) {
        i += 1;
        return c;
    } else if ((c & 0xE0) == 0xC0 && cont(1)) {
        cp = ((c & 0x1F) << 6) | (s[i + 1] & 0x3F);
        i += 2;
    } else if ((c & 0xF0) == 0xE0 && cont(1) && cont(2)) {
        cp = ((c & 0x0F) << 12) | ((s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F);
        i += 3;
    } else if ((c & 0xF8) == 0xF0 && cont(1) && cont(2) && cont(3)) {
        cp = ((c & 0x07) << 18) | ((s[i + 1] & 0x3F) << 12) | ((s[i + 2] & 0x3F) << 6) | (s[i + 3] & 0x3F);
        i += 4;
    } else {
        i += 1;
        return 0xFFFD;
    }
    return cp;
}

void AppendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += char(cp);
    } else if (cp < 0x800) {
        out += char(0xC0 | (cp >> 6));
        out += char(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += char(0xE0 | (cp >> 12));
        out += char(0x80 | ((cp >> 6) & 0x3F));
        out += char(0x80 | (cp & 0x3F));
    } else {
        out += char(0xF0 | (cp >> 18));
        out += char(0x80 | ((cp >> 12) & 0x3F));
        out += char(0x80 | ((cp >> 6) & 0x3F));
        out += char(0x80 | (cp & 0x3F));
    }
}

std::string TwemojiName(std::string_view emoji) {
    bool zwj = emoji.find("\xE2\x80\x8D") != std::string_view::npos;
    std::string out;
    char buf[16];
    for (size_t i = 0; i < emoji.size();) {
        uint32_t cp = DecodeUtf8(emoji, i);
        if (cp == 0xFE0F && !zwj) continue;
        snprintf(buf, sizeof(buf), "%x", cp);
        if (!out.empty()) out += '-';
        out += buf;
    }
    return out;
}

namespace {

// emojiDefault is the code points below U+1F000 that are emoji without
// U+FE0F after them: the table's emoji that are one such code point alone.
const std::unordered_set<uint32_t>& emojiDefault() {
    static const auto set = [] {
        std::unordered_set<uint32_t> s;
        for (const auto& e : EmojiTable()) {
            std::string_view v = e.emoji;
            size_t i = 0;
            uint32_t cp = DecodeUtf8(v, i);
            if (i == v.size() && cp < 0x1F000) s.insert(cp);
        }
        return s;
    }();
    return set;
}

bool isEmojiBase(uint32_t cp) { return (cp >= 0x1F000 && cp <= 0x1FAFF) && !(cp >= 0x1F3FB && cp <= 0x1F3FF); }

// textSymbol is a code point that's an emoji only when U+FE0F follows.
bool textSymbol(uint32_t cp) {
    return cp == 0xA9 || cp == 0xAE || (cp >= 0x2000 && cp <= 0x3300);
}

}  // namespace

size_t EmojiAt(std::string_view s, size_t i, std::string* file) {
    size_t start = i, j = i;
    if (j >= s.size()) return 0;
    uint32_t cp = DecodeUtf8(s, j);
    auto peek = [&](size_t at, size_t* next) -> uint32_t {
        if (at >= s.size()) return 0;
        size_t k = at;
        uint32_t c = DecodeUtf8(s, k);
        if (next) *next = k;
        return c;
    };
    size_t k;
    // Keycaps: 0-9, # or * with U+FE0F (optional) and U+20E3.
    if ((cp >= '0' && cp <= '9') || cp == '#' || cp == '*') {
        size_t at = j;
        if (peek(at, &k) == 0xFE0F) at = k;
        if (peek(at, &k) != 0x20E3) return 0;
        j = k;
    } else if (cp >= 0x1F1E6 && cp <= 0x1F1FF) {
        // Flags are pairs of regional indicators.
        uint32_t next = peek(j, &k);
        if (next >= 0x1F1E6 && next <= 0x1F1FF) j = k;
    } else if (isEmojiBase(cp) || emojiDefault().count(cp)) {
        // An emoji already.
    } else if (textSymbol(cp) && peek(j, &k) == 0xFE0F) {
        j = k;
    } else {
        return 0;
    }
    // Modifiers, tags and ZWJ joins.
    for (;;) {
        uint32_t next = peek(j, &k);
        if (next == 0xFE0F || (next >= 0x1F3FB && next <= 0x1F3FF) || next == 0x20E3 ||
            (next >= 0xE0020 && next <= 0xE007F)) {
            j = k;
            continue;
        }
        if (next == 0x200D) {
            size_t k2 = 0;
            uint32_t after = peek(k, &k2);
            if (isEmojiBase(after) || emojiDefault().count(after) || (textSymbol(after) && after != 0x200D)) {
                j = k2;
                continue;
            }
        }
        break;
    }
    if (file) *file = TwemojiName(s.substr(start, j - start));
    return j - start;
}

namespace {

class Inline {
public:
    Inline(std::vector<Span>& out, int& spoilers) : out_(out), spoilers_(spoilers) {}

    void run(std::string_view s, uint16_t st, int spoiler) {
        size_t i = 0;
        while (i < s.size()) {
            char c = s[i];
            std::string_view rest = s.substr(i);
            // Escapes: a backslash before punctuation shows it as is.
            if (c == '\\' && i + 1 < s.size() && ispunct((unsigned char)s[i + 1])) {
                text(s.substr(i + 1, 1), st, spoiler);
                i += 2;
                continue;
            }
            if (c == '`') {
                size_t ticks = rest.rfind("``", 0) == 0 ? 2 : 1;
                std::string_view delim = rest.substr(0, ticks);
                size_t close = s.find(delim, i + ticks);
                if (close != std::string_view::npos && close > i + ticks) {
                    std::string_view code = s.substr(i + ticks, close - i - ticks);
                    if (ticks == 2 && code.size() > 2 && code.front() == ' ' && code.back() == ' ')
                        code = code.substr(1, code.size() - 2);
                    text(code, st | style::Code, spoiler, true);
                    i = close + ticks;
                    continue;
                }
            }
            if (c == '<' && angle(s, i, st, spoiler)) continue;
            if (c == '[' && masked(s, i, st, spoiler)) continue;
            if (pair(s, i, "**", style::Bold, st, spoiler)) continue;
            if (pair(s, i, "__", style::Underline, st, spoiler)) continue;
            if (pair(s, i, "~~", style::Strike, st, spoiler)) continue;
            if (rest.rfind("||", 0) == 0) {
                size_t close = findClose(s, i + 2, "||");
                if (close != std::string_view::npos && close > i + 2) {
                    int n = ++spoilers_;
                    run(s.substr(i + 2, close - i - 2), st | style::Spoiler, n);
                    i = close + 2;
                    continue;
                }
            }
            if (c == '*' && italic(s, i, '*', st, spoiler)) continue;
            if (c == '_' && (i == 0 || !isalnum((unsigned char)s[i - 1])) && italic(s, i, '_', st, spoiler)) continue;
            if ((rest.rfind("https://", 0) == 0 || rest.rfind("http://", 0) == 0) &&
                (i == 0 || !isalnum((unsigned char)s[i - 1]))) {
                size_t end = i;
                while (end < s.size() && !isspace((unsigned char)s[end]) && s[end] != '<') end++;
                // Trailing punctuation belongs to the sentence, and a ')'
                // only to the URL if it opened one.
                while (end > i) {
                    char t = s[end - 1];
                    if (strchr(".,:;\"'!?", t)) {
                        end--;
                    } else if (t == ')') {
                        std::string_view u = s.substr(i, end - i);
                        if (std::count(u.begin(), u.end(), '(') < std::count(u.begin(), u.end(), ')'))
                            end--;
                        else
                            break;
                    } else {
                        break;
                    }
                }
                std::string url(s.substr(i, end - i));
                if (url.size() > 8) {
                    Span sp{Span::Kind::Link};
                    sp.text = sp.url = url;
                    push(sp, st, spoiler);
                    i = end;
                    continue;
                }
            }
            if (c == '@' && (rest.rfind("@everyone", 0) == 0 || rest.rfind("@here", 0) == 0)) {
                Span sp{Span::Kind::Everyone};
                sp.text = rest.rfind("@everyone", 0) == 0 ? "@everyone" : "@here";
                push(sp, st, spoiler);
                i += sp.text.size();
                continue;
            }
            if ((unsigned char)c >= 0x80 || c == '#' || c == '*' || isdigit((unsigned char)c)) {
                std::string file;
                if (size_t n = EmojiAt(s, i, &file)) {
                    Span sp{Span::Kind::Emoji};
                    sp.text = std::string(s.substr(i, n));
                    sp.file = file;
                    push(sp, st, spoiler);
                    i += n;
                    continue;
                }
            }
            // A plain character, whole if it's multi-byte.
            size_t j = i;
            DecodeUtf8(s, j);
            text(s.substr(i, j - i), st, spoiler);
            i = j;
        }
    }

private:
    void push(Span sp, uint16_t st, int spoiler) {
        sp.style = st;
        sp.spoiler = spoiler;
        out_.push_back(std::move(sp));
    }

    void text(std::string_view t, uint16_t st, int spoiler, bool separate = false) {
        if (!separate && !out_.empty() && out_.back().kind == Span::Kind::Text && out_.back().style == st &&
            out_.back().spoiler == spoiler && !lastSeparate_) {
            out_.back().text.append(t);
            return;
        }
        Span sp;
        sp.text = std::string(t);
        push(sp, st, spoiler);
        lastSeparate_ = separate;
    }

    // findClose finds a closing delimiter, skipping escaped characters and
    // inline code.
    static size_t findClose(std::string_view s, size_t from, std::string_view delim) {
        for (size_t i = from; i < s.size(); i++) {
            if (s[i] == '\\') {
                i++;
                continue;
            }
            if (s[i] == '`') {
                size_t close = s.find('`', i + 1);
                if (close == std::string_view::npos) continue;
                i = close;
                continue;
            }
            if (s.compare(i, delim.size(), delim) == 0) return i;
        }
        return std::string_view::npos;
    }

    bool pair(std::string_view s, size_t& i, std::string_view delim, uint16_t add, uint16_t st, int spoiler) {
        if (s.compare(i, delim.size(), delim) != 0) return false;
        size_t close = findClose(s, i + delim.size(), delim);
        if (close == std::string_view::npos || close == i + delim.size()) return false;
        // With a run like ***x***, the closing pair is the last two.
        while (close + delim.size() < s.size() && s[close + delim.size()] == delim[0]) close++;
        run(s.substr(i + delim.size(), close - i - delim.size()), st | add, spoiler);
        i = close + delim.size();
        return true;
    }

    bool italic(std::string_view s, size_t& i, char d, uint16_t st, int spoiler) {
        if (i + 1 >= s.size() || isspace((unsigned char)s[i + 1])) return false;
        for (size_t close = i + 1; close < s.size(); close++) {
            if (s[close] == '\\') {
                close++;
                continue;
            }
            if (s[close] != d) continue;
            if (d == '*' && close + 1 < s.size() && s[close + 1] == '*') {
                close++;  // part of a **
                continue;
            }
            if (isspace((unsigned char)s[close - 1])) continue;
            if (d == '_' && close + 1 < s.size() && isalnum((unsigned char)s[close + 1])) continue;
            if (close == i + 1) return false;
            run(s.substr(i + 1, close - i - 1), st | style::Italic, spoiler);
            i = close + 1;
            return true;
        }
        return false;
    }

    static bool digitsOnly(std::string_view v) {
        return !v.empty() && std::all_of(v.begin(), v.end(), [](char c) { return isdigit((unsigned char)c); });
    }

    // angle reads <@id>, <@!id>, <@&id>, <#id>, <:name:id>, <a:name:id>,
    // <t:time:f>, </command:id> and <url>.
    bool angle(std::string_view s, size_t& i, uint16_t st, int spoiler) {
        size_t close = s.find('>', i);
        if (close == std::string_view::npos) return false;
        std::string_view in = s.substr(i + 1, close - i - 1);
        Span sp;
        auto num = [](std::string_view v) { return std::strtoull(std::string(v).c_str(), nullptr, 10); };
        if (in.rfind("@&", 0) == 0 && digitsOnly(in.substr(2))) {
            sp.kind = Span::Kind::RoleMention;
            sp.id = num(in.substr(2));
        } else if (in.rfind("@!", 0) == 0 && digitsOnly(in.substr(2))) {
            sp.kind = Span::Kind::UserMention;
            sp.id = num(in.substr(2));
        } else if (in.rfind("@", 0) == 0 && digitsOnly(in.substr(1))) {
            sp.kind = Span::Kind::UserMention;
            sp.id = num(in.substr(1));
        } else if (in.rfind("#", 0) == 0 && digitsOnly(in.substr(1))) {
            sp.kind = Span::Kind::ChannelMention;
            sp.id = num(in.substr(1));
        } else if (in.rfind(":", 0) == 0 || in.rfind("a:", 0) == 0) {
            bool animated = in[0] == 'a';
            std::string_view body = in.substr(animated ? 2 : 1);
            size_t colon = body.find(':');
            if (colon == std::string_view::npos || colon == 0 || !digitsOnly(body.substr(colon + 1))) return false;
            sp.kind = Span::Kind::CustomEmoji;
            sp.text = std::string(body.substr(0, colon));
            sp.id = num(body.substr(colon + 1));
            sp.animated = animated;
        } else if (in.rfind("t:", 0) == 0) {
            std::string_view body = in.substr(2);
            size_t colon = body.find(':');
            std::string_view t = body.substr(0, colon);
            if (!digitsOnly(t) && !(t.size() > 1 && t[0] == '-' && digitsOnly(t.substr(1)))) return false;
            sp.kind = Span::Kind::Timestamp;
            sp.time = std::strtoll(std::string(t).c_str(), nullptr, 10);
            if (colon != std::string_view::npos) {
                std::string_view f = body.substr(colon + 1);
                if (f.size() != 1 || !strchr("tTdDfFR", f[0])) return false;
                sp.format = f[0];
            }
        } else if (in.rfind("/", 0) == 0) {
            size_t colon = in.find(':');
            if (colon == std::string_view::npos || !digitsOnly(in.substr(colon + 1))) return false;
            sp.kind = Span::Kind::Command;
            sp.text = std::string(in.substr(0, colon));
        } else if ((in.rfind("https://", 0) == 0 || in.rfind("http://", 0) == 0) &&
                   in.find(' ') == std::string_view::npos) {
            sp.kind = Span::Kind::Link;
            sp.text = sp.url = std::string(in);
        } else {
            return false;
        }
        push(sp, st, spoiler);
        i = close + 1;
        return true;
    }

    // masked reads [text](url): a link shown as its text.
    bool masked(std::string_view s, size_t& i, uint16_t st, int spoiler) {
        size_t mid = s.find("](", i);
        if (mid == std::string_view::npos) return false;
        std::string_view label = s.substr(i + 1, mid - i - 1);
        if (label.empty() || label.find('[') != std::string_view::npos) return false;
        size_t close = s.find(')', mid + 2);
        if (close == std::string_view::npos) return false;
        std::string_view url = s.substr(mid + 2, close - mid - 2);
        if (url.size() > 2 && url.front() == '<' && url.back() == '>') url = url.substr(1, url.size() - 2);
        if ((url.rfind("https://", 0) != 0 && url.rfind("http://", 0) != 0) || url.find(' ') != std::string_view::npos)
            return false;
        Span sp{Span::Kind::Link};
        sp.text = std::string(label);
        sp.url = std::string(url);
        push(sp, st, spoiler);
        i = close + 1;
        return true;
    }

    std::vector<Span>& out_;
    int& spoilers_;
    bool lastSeparate_ = false;
};

bool startsWith(std::string_view s, std::string_view p) { return s.substr(0, p.size()) == p; }

void textLines(std::string_view s, std::vector<Para>& out, int& spoilers, bool& quoteRest) {
    size_t pos = 0;
    while (pos <= s.size()) {
        size_t nl = s.find('\n', pos);
        std::string_view line = s.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
        Para p;
        if (!quoteRest && startsWith(line, ">>> ")) {
            quoteRest = true;
            line.remove_prefix(4);
        } else if (startsWith(line, "> ")) {
            p.quote = true;
            line.remove_prefix(2);
        } else if (line == ">") {
            p.quote = true;
            line = {};
        }
        if (quoteRest) p.quote = true;
        if (startsWith(line, "# ")) {
            p.heading = 1;
            line.remove_prefix(2);
        } else if (startsWith(line, "## ")) {
            p.heading = 2;
            line.remove_prefix(3);
        } else if (startsWith(line, "### ")) {
            p.heading = 3;
            line.remove_prefix(4);
        } else if (startsWith(line, "-# ")) {
            p.subtext = true;
            line.remove_prefix(3);
        } else {
            size_t spaces = 0;
            while (spaces < line.size() && line[spaces] == ' ') spaces++;
            std::string_view item = line.substr(spaces);
            if (startsWith(item, "- ") || startsWith(item, "* ")) {
                p.list = int(spaces / 2) + 1;
                p.marker = "\xE2\x80\xA2";  // •
                line = item.substr(2);
            } else {
                size_t d = 0;
                while (d < item.size() && d < 9 && isdigit((unsigned char)item[d])) d++;
                if (d > 0 && startsWith(item.substr(d), ". ")) {
                    p.list = int(spaces / 2) + 1;
                    p.marker = std::string(item.substr(0, d + 1));
                    line = item.substr(d + 2);
                }
            }
        }
        Inline(p.spans, spoilers).run(line, 0, 0);
        out.push_back(std::move(p));
        if (nl == std::string_view::npos) break;
        pos = nl + 1;
    }
}

}  // namespace

std::vector<Para> Parse(std::string_view content, bool* jumbo) {
    std::vector<Para> out;
    int spoilers = 0;
    bool quoteRest = false;
    size_t pos = 0;
    while (pos < content.size()) {
        size_t open = content.find("```", pos);
        size_t close = open == std::string_view::npos ? open : content.find("```", open + 3);
        if (close == std::string_view::npos) {
            textLines(content.substr(pos), out, spoilers, quoteRest);
            break;
        }
        if (open > pos) {
            std::string_view before = content.substr(pos, open - pos);
            if (!before.empty() && before.back() == '\n') before.remove_suffix(1);
            textLines(before, out, spoilers, quoteRest);
        }
        std::string_view inner = content.substr(open + 3, close - open - 3);
        Para p;
        p.code = true;
        p.quote = quoteRest;
        size_t nl = inner.find('\n');
        if (nl != std::string_view::npos) {
            std::string_view first = inner.substr(0, nl);
            bool word = !first.empty() && std::all_of(first.begin(), first.end(), [](char c) {
                return isalnum((unsigned char)c) || c == '+' || c == '-' || c == '#' || c == '_' || c == '.';
            });
            if (word || first.empty()) {
                p.lang = std::string(first);
                inner.remove_prefix(nl + 1);
            }
        }
        if (!inner.empty() && inner.back() == '\n') inner.remove_suffix(1);
        p.text = std::string(inner);
        out.push_back(std::move(p));
        pos = close + 3;
        if (pos < content.size() && content[pos] == '\n') pos++;
    }
    if (jumbo) {
        int emoji = 0;
        bool only = !out.empty();
        for (const auto& p : out) {
            if (p.code) only = false;
            for (const auto& sp : p.spans) {
                if (sp.kind == Span::Kind::Emoji || sp.kind == Span::Kind::CustomEmoji)
                    emoji++;
                else if (sp.kind != Span::Kind::Text || sp.style ||
                         !std::all_of(sp.text.begin(), sp.text.end(), [](char c) { return isspace((unsigned char)c); }))
                    only = false;
            }
        }
        *jumbo = only && emoji > 0 && emoji <= 30;
    }
    return out;
}

std::string StripAnsi(std::string_view s, std::vector<AnsiRun>* runs) {
    std::string out;
    AnsiRun cur;
    auto flush = [&] {
        cur.end = out.size();
        if (runs && cur.end > cur.start) runs->push_back(cur);
        cur.start = out.size();
    };
    for (size_t i = 0; i < s.size();) {
        if (s[i] != '\x1b' || i + 1 >= s.size() || s[i + 1] != '[') {
            out += s[i++];
            continue;
        }
        size_t j = i + 2;
        while (j < s.size() && (isdigit((unsigned char)s[j]) || s[j] == ';')) j++;
        if (j >= s.size() || s[j] != 'm') {
            out += s[i++];  // not an SGR escape: shown as it is
            continue;
        }
        flush();
        std::string_view params = s.substr(i + 2, j - i - 2);
        i = j + 1;
        if (params.empty()) params = "0";
        for (size_t a = 0; a <= params.size();) {
            size_t b = params.find(';', a);
            if (b == std::string_view::npos) b = params.size();
            int n = 0;
            for (size_t k = a; k < b; k++) n = n * 10 + (params[k] - '0');
            if (n == 0) {
                cur.fg = cur.bg = 0;
                cur.bold = cur.underline = false;
            } else if (n == 1) {
                cur.bold = true;
            } else if (n == 4) {
                cur.underline = true;
            } else if (n >= 30 && n <= 37) {
                cur.fg = n;
            } else if (n >= 40 && n <= 47) {
                cur.bg = n;
            }
            a = b + 1;
        }
    }
    flush();
    return out;
}

}  // namespace who
