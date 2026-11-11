#include "richtext.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace who {

namespace {

ImFont* fontFor(const Fonts& f, uint16_t st, bool heading) {
    if (st & style::Code) return f.mono;
    bool bold = (st & style::Bold) || heading, it = st & style::Italic;
    if (bold && it) return f.boldItalic;
    if (bold) return f.bold;
    if (it) return f.italic;
    return f.regular;
}

// ansiColour is Discord's colour for an ```ansi SGR code (30-37, 40-47).
// White text follows the theme's text colour, so it shows in light mode.
ImU32 ansiColour(int code) {
    static const unsigned fg[8] = {0x4f545c, 0xdc322f, 0x859900, 0xb58900, 0x268bd2, 0xd33682, 0x2aa198, 0xffffff};
    static const unsigned bg[8] = {0x002b36, 0xcb4b16, 0x586e75, 0x657b83, 0x839496, 0x6c71c4, 0x93a1a1, 0xfdf6e3};
    if (code == 37) return U32(Colours().text);
    if (code >= 30 && code <= 37) return U32(Hex(fg[code - 30]));
    if (code >= 40 && code <= 47) return U32(Hex(bg[code - 40]));
    return 0;
}

bool isAnsi(const std::string& lang) {
    return lang.size() == 4 && std::equal(lang.begin(), lang.end(), "ansi",
                                          [](char a, char b) { return std::tolower((unsigned char)a) == b; });
}

std::string twemoji(const std::string& file) {
    return "https://cdn.jsdelivr.net/gh/jdecked/twemoji@15.1.0/assets/72x72/" + file + ".png";
}

// customEmoji is a custom emoji's image; animated ones are GIFs, which
// show their first frame until they're played.
std::string customEmoji(uint64_t id, bool animated) {
    return "https://cdn.discordapp.com/emojis/" + std::to_string(id) + (animated ? ".gif" : ".png") + "?size=48";
}

class Builder {
public:
    Builder(Layout& out, float width, const Fonts& fonts, const Resolver& names, float scale)
        : out_(out), width_(width), fonts_(fonts), names_(names), scale_(scale) {}

    void para(const Para& p, bool jumbo, float body, bool muted) {
        const Palette& c = Colours();
        float size = body;
        if (p.heading == 1) size = kH1;
        if (p.heading == 2) size = kH2;
        if (p.heading == 3) size = kH3;
        if (p.subtext) size = kSmall;
        size_ = size * scale_;
        lineH_ = size_ * 1.3f;
        emoji_ = jumbo ? kJumbo * scale_ : size_ * 1.375f;
        if (jumbo) lineH_ = emoji_ + 4 * scale_;
        heading_ = p.heading > 0;
        baseColour_ = U32(muted || p.subtext ? c.muted : p.heading ? c.heading : c.text);
        if (p.heading && y_ > 0) y_ += 8 * scale_;
        indent_ = 0;
        float top = y_;
        if (p.quote) indent_ += 16 * scale_;
        if (p.list) {
            float markerX = indent_ + (p.list - 1) * 16 * scale_;
            indent_ = markerX + 18 * scale_;
            Piece m;
            m.text = p.marker;
            m.font = fonts_.regular;
            m.fontSize = size_;
            m.colour = baseColour_;
            ImVec2 sz = m.font->CalcTextSizeA(size_, FLT_MAX, 0, m.text.c_str());
            m.pos = ImVec2(markerX, y_ + (lineH_ - sz.y) / 2);
            m.size = sz;
            out_.pieces.push_back(m);
        }
        x_ = indent_;
        lineTop_ = y_;
        curLineH_ = lineH_;
        if (p.code) {
            codeBlock(p);
        } else {
            for (size_t i = 0; i < p.spans.size(); i++) span(p.spans[i], int(i));
            newline();
        }
        if (p.quote) {
            Piece bar;
            bar.kind = Piece::Kind::Bar;
            bar.pos = ImVec2(0, top);
            bar.size = ImVec2(4 * scale_, y_ - top);
            bar.colour = U32(c.muted, 0.6f);
            out_.pieces.push_back(bar);
        }
    }

    void finish() {
        out_.height = y_;
        float w = 0;
        for (const auto& p : out_.pieces) w = std::max(w, p.pos.x + p.size.x);
        out_.width = w;
    }

private:
    void newline() {
        y_ = lineTop_ + curLineH_;
        lineTop_ = y_;
        curLineH_ = lineH_;
        x_ = indent_;
        lastSpan_ = -1;
    }

    void codeBlock(const Para& p) {
        const Palette& c = Colours();
        float pad = 8 * scale_, size = 14 * scale_;
        float top = y_ + 2 * scale_;
        Piece box;
        box.kind = Piece::Kind::Box;
        box.colour = U32(c.code);
        size_t boxAt = out_.pieces.size();
        out_.pieces.push_back(box);
        float inner = std::max(40.0f, width_ - indent_ - pad * 2);
        float yy = top + pad;
        float lh = size * 1.3f;
        size_t pos = 0;
        std::vector<AnsiRun> runs;
        bool ansi = isAnsi(p.lang);
        const std::string t = ansi ? StripAnsi(p.text, &runs) : p.text;
        float widest = 0;
        while (pos <= t.size()) {
            size_t nl = t.find('\n', pos);
            if (nl == std::string::npos) nl = t.size();
            const char* s = t.c_str() + pos;
            const char* e = t.c_str() + nl;
            do {
                const char* wrap = fonts_.mono->CalcWordWrapPosition(size, s, e, inner);
                if (wrap == s && s < e) wrap = s + 1;
                if (ansi) {
                    widest = std::max(widest, ansiLine(runs, t, size_t(s - t.c_str()), size_t(wrap - t.c_str()),
                                                       indent_ + pad, yy, size));
                } else {
                    Piece line;
                    line.text.assign(s, wrap);
                    NoteText(line.text);
                    line.font = fonts_.mono;
                    line.fontSize = size;
                    line.colour = U32(c.text);
                    line.size = fonts_.mono->CalcTextSizeA(size, FLT_MAX, 0, line.text.c_str());
                    line.pos = ImVec2(indent_ + pad, yy);
                    widest = std::max(widest, line.size.x);
                    out_.pieces.push_back(line);
                }
                yy += lh;
                s = wrap;
            } while (s < e);
            if (nl >= t.size()) break;
            pos = nl + 1;
        }
        Piece& b = out_.pieces[boxAt];
        b.pos = ImVec2(indent_, top);
        b.size = ImVec2(std::max(width_ - indent_, widest + pad * 2), yy - top + pad - (lh - size));
        y_ = b.pos.y + b.size.y + 4 * scale_;
        lineTop_ = y_;
    }

    // ansiLine lays out t[from, to), one line of an ```ansi block, as a
    // piece per styled run; it returns the line's width.
    float ansiLine(const std::vector<AnsiRun>& runs, const std::string& t, size_t from, size_t to, float x, float y,
                   float size) {
        float x0 = x;
        for (const auto& r : runs) {
            size_t a = std::max(r.start, from), b = std::min(r.end, to);
            if (a >= b) continue;
            Piece piece;
            piece.text = t.substr(a, b - a);
            NoteText(piece.text);
            piece.font = fonts_.mono;
            piece.fontSize = size;
            piece.colour = r.fg ? ansiColour(r.fg) : U32(Colours().text);
            piece.back = r.bg ? ansiColour(r.bg) : 0;
            piece.thicken = r.bold;
            if (r.underline) piece.style |= style::Underline;
            piece.size = fonts_.mono->CalcTextSizeA(size, FLT_MAX, 0, piece.text.c_str());
            piece.pos = ImVec2(x, y);
            x += piece.size.x;
            out_.pieces.push_back(std::move(piece));
        }
        return x - x0;
    }

    // place puts a piece of width w on the line, wrapping first if it
    // doesn't fit and the line already has something on it.
    void place(Piece& p, float w, float h) {
        if (x_ + w > width_ && x_ > indent_) newline();
        if (h > curLineH_) curLineH_ = h;
        p.pos = ImVec2(x_, lineTop_ + (lineH_ - h) / 2);
        p.size = ImVec2(w, h);
        x_ += w;
    }

    void text(const std::string& t, const Span& sp, int index, ImU32 colour, bool pill = false,
              const std::string& link = {}, uint64_t channel = 0) {
        ImFont* font = fontFor(fonts_, sp.style, heading_);
        float size = (sp.style & style::Code) ? size_ * 0.875f : size_;
        // Words with their trailing spaces, so lines break between them.
        size_t pos = 0;
        while (pos < t.size()) {
            size_t end = pos;
            while (end < t.size() && t[end] != ' ') end++;
            while (end < t.size() && t[end] == ' ') end++;
            std::string word = t.substr(pos, end - pos);
            pos = end;
            float w = font->CalcTextSizeA(size, FLT_MAX, 0, word.c_str()).x;
            // A word too long for any line is broken where it must be.
            float avail = width_ - indent_;
            if (w > avail && !pill) {
                const char* s = word.c_str();
                const char* e = s + word.size();
                while (s < e) {
                    float room = width_ - x_;
                    if (room < size) {
                        newline();
                        room = width_ - x_;
                    }
                    const char* cut = font->CalcWordWrapPosition(size, s, e, room);
                    if (cut == s) cut = s + 1;
                    emit(std::string(s, cut), font, size, sp, index, colour, pill, link, channel);
                    s = cut;
                    if (s < e) newline();
                }
                continue;
            }
            if (x_ + w > width_ && x_ > indent_) newline();
            emit(word, font, size, sp, index, colour, pill, link, channel);
        }
    }

    void emit(const std::string& word, ImFont* font, float size, const Span& sp, int index, ImU32 colour, bool pill,
              const std::string& link, uint64_t channel) {
        NoteText(word);
        ImVec2 sz = font->CalcTextSizeA(size, FLT_MAX, 0, word.c_str());
        // Join to the last piece if it's the same span on the same line.
        if (lastSpan_ == index && !out_.pieces.empty() && out_.pieces.back().kind == Piece::Kind::Text &&
            out_.pieces.back().pos.y == lineTop_ + (lineH_ - sz.y) / 2 && x_ + sz.x <= width_ + 0.5f) {
            Piece& last = out_.pieces.back();
            last.text += word;
            last.size.x += sz.x;
            x_ += sz.x;
            return;
        }
        Piece p;
        p.text = word;
        p.font = font;
        p.fontSize = size;
        p.colour = colour;
        p.style = sp.style;
        p.spoiler = sp.spoiler;
        p.pill = pill;
        p.link = link;
        p.channel = channel;
        place(p, sz.x, sz.y);
        p.pos.y = lineTop_ + (lineH_ - sz.y) / 2;
        out_.pieces.push_back(p);
        lastSpan_ = index;
    }

    void image(const std::string& url, const std::string& fallback, const Span& sp) {
        Piece p;
        p.kind = Piece::Kind::Image;
        p.image = url;
        p.fallback = fallback;
        p.spoiler = sp.spoiler;
        p.font = fonts_.regular;
        p.fontSize = size_;
        float gap = 1 * scale_;
        place(p, emoji_ + gap * 2, emoji_);
        p.pos.x += gap;
        p.size.x = emoji_;
        out_.pieces.push_back(p);
        lastSpan_ = -1;
    }

    void span(const Span& sp, int index) {
        const Palette& c = Colours();
        ImU32 col = baseColour_;
        switch (sp.kind) {
        case Span::Kind::Text: text(sp.text, sp, index, col); break;
        case Span::Kind::Emoji: image(twemoji(sp.file), sp.text, sp); break;
        case Span::Kind::CustomEmoji: image(customEmoji(sp.id, sp.animated), ":" + sp.text + ":", sp); break;
        case Span::Kind::UserMention:
            text("@" + names_.user(sp.id), sp, index, U32(ImVec4(0.79f, 0.80f, 0.98f, 1)), true);
            break;
        case Span::Kind::RoleMention: {
            ImU32 rc = names_.roleColour ? names_.roleColour(sp.id) : 0;
            text("@" + names_.role(sp.id), sp, index, rc ? rc : U32(ImVec4(0.79f, 0.80f, 0.98f, 1)), true);
            break;
        }
        case Span::Kind::ChannelMention:
            text("#" + names_.channel(sp.id), sp, index, U32(ImVec4(0.79f, 0.80f, 0.98f, 1)), true, {}, sp.id);
            break;
        case Span::Kind::Everyone:
            text(sp.text, sp, index, U32(ImVec4(0.79f, 0.80f, 0.98f, 1)), true);
            break;
        case Span::Kind::Command: text(sp.text, sp, index, U32(c.link)); break;
        case Span::Kind::Link: text(sp.text, sp, index, U32(c.link), false, sp.url); break;
        case Span::Kind::Timestamp: {
            text(names_.time ? names_.time(sp.time, sp.format) : std::to_string(sp.time), sp, index, col, true);
            break;
        }
        }
    }

    Layout& out_;
    float width_;
    const Fonts& fonts_;
    const Resolver& names_;
    float scale_;
    float x_ = 0, y_ = 0, lineTop_ = 0, lineH_ = 0, curLineH_ = 0, indent_ = 0, size_ = 0, emoji_ = 0;
    bool heading_ = false;
    ImU32 baseColour_ = 0;
    int lastSpan_ = -1;
};

}  // namespace

Layout LayOut(const std::vector<Para>& paras, float width, const Fonts& fonts, const Resolver& names, float scale,
              bool jumbo, float bodySize, bool muted) {
    Layout out;
    Builder b(out, std::max(width, 50.0f), fonts, names, scale);
    for (const auto& p : paras) b.para(p, jumbo, bodySize, muted);
    b.finish();
    return out;
}

Hit Draw(const Layout& l, ImVec2 at, const ImageFor& images, const std::vector<int>& revealed) {
    const Palette& c = Colours();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 mouse = ImGui::GetMousePos();
    bool windowHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    Hit hit;
    for (const auto& p : l.pieces) {
        ImVec2 a(at.x + p.pos.x, at.y + p.pos.y), b(a.x + p.size.x, a.y + p.size.y);
        bool over = windowHovered && mouse.x >= a.x && mouse.x < b.x && mouse.y >= a.y && mouse.y < b.y;
        if (over && p.kind != Piece::Kind::Box && p.kind != Piece::Kind::Bar) hit.piece = &p;
        bool hidden = p.spoiler && std::find(revealed.begin(), revealed.end(), p.spoiler) == revealed.end();
        switch (p.kind) {
        case Piece::Kind::Box: dl->AddRectFilled(a, b, p.colour); break;
        case Piece::Kind::Bar: dl->AddRectFilled(a, b, p.colour); break;
        case Piece::Kind::Image: {
            if (hidden) {
                dl->AddRectFilled(a, b, U32(c.spoiler));
                break;
            }
            ImTextureID tex = images ? images(p.image) : ImTextureID(0);
            if (tex)
                dl->AddImage(ImTextureRef(tex), a, b);
            else
                dl->AddText(p.font, p.fontSize, a, U32(c.muted), p.fallback.c_str());
            break;
        }
        case Piece::Kind::Text: {
            float pad = 2;
            if (hidden) {
                dl->AddRectFilled(ImVec2(a.x - 1, a.y), ImVec2(b.x + 1, b.y), U32(c.spoiler));
                break;
            }
            if (p.spoiler) dl->AddRectFilled(ImVec2(a.x - 1, a.y), ImVec2(b.x + 1, b.y), U32(c.text, 0.1f));
            if (p.pill)
                dl->AddRectFilled(ImVec2(a.x - pad, a.y), ImVec2(b.x + pad, b.y), U32(c.accent, over ? 0.6f : 0.3f));
            if (p.style & style::Code) dl->AddRectFilled(ImVec2(a.x - pad, a.y - 1), ImVec2(b.x + pad, b.y + 1), U32(c.code));
            if (p.back) dl->AddRectFilled(a, b, p.back);
            dl->AddText(p.font, p.fontSize, a, p.colour, p.text.c_str());
            if (p.thicken) dl->AddText(p.font, p.fontSize, ImVec2(a.x + 1, a.y), p.colour, p.text.c_str());
            float th = std::max(1.0f, p.fontSize / 16);
            if ((p.style & style::Underline) || (!p.link.empty() && over))
                dl->AddRectFilled(ImVec2(a.x, b.y - th), ImVec2(b.x, b.y), p.colour);
            if (p.style & style::Strike) {
                float mid = (a.y + b.y) / 2 + th / 2;
                dl->AddRectFilled(ImVec2(a.x, mid - th), ImVec2(b.x, mid), p.colour);
            }
            break;
        }
        }
    }
    return hit;
}

float Label(ImDrawList* dl, ImFont* font, float size, ImVec2 at, ImU32 colour, const std::string& text, float maxWidth,
            const ImageFor& images) {
    struct Part {
        std::string text, file;
        float w;
    };
    NoteText(text);
    std::vector<Part> parts;
    float emoji = size * 1.15f, total = 0;
    for (size_t i = 0; i < text.size();) {
        std::string file;
        if (size_t n = EmojiAt(text, i, &file)) {
            parts.push_back({text.substr(i, n), file, emoji + 2});
            i += n;
            continue;
        }
        size_t j = i;
        DecodeUtf8(text, j);
        if (parts.empty() || !parts.back().file.empty()) parts.push_back({"", "", 0});
        parts.back().text += text.substr(i, j - i);
        i = j;
    }
    for (auto& p : parts)
        if (p.file.empty()) p.w = font->CalcTextSizeA(size, FLT_MAX, 0, p.text.c_str()).x;
    for (const auto& p : parts) total += p.w;
    const char* ellipsis = "\xE2\x80\xA6";
    float ew = font->CalcTextSizeA(size, FLT_MAX, 0, ellipsis).x;
    bool cut = maxWidth > 0 && total > maxWidth;
    float limit = cut ? maxWidth - ew : FLT_MAX, x = 0;
    for (const auto& p : parts) {
        if (!p.file.empty()) {
            if (x + p.w > limit) break;
            if (dl) {
                ImTextureID t = images ? images("https://cdn.jsdelivr.net/gh/jdecked/twemoji@15.1.0/assets/72x72/" + p.file + ".png")
                                       : ImTextureID(0);
                ImVec2 a(at.x + x + 1, at.y + (size - emoji) / 2 + size * 0.1f);
                if (t) dl->AddImage(ImTextureRef(t), a, ImVec2(a.x + emoji, a.y + emoji));
            }
            x += p.w;
            continue;
        }
        if (x + p.w <= limit) {
            if (dl) dl->AddText(font, size, ImVec2(at.x + x, at.y), colour, p.text.c_str());
            x += p.w;
            continue;
        }
        // Cut this run where it stops fitting, by whole characters.
        std::string shown;
        float w = 0;
        for (size_t i = 0; i < p.text.size();) {
            size_t j = i;
            DecodeUtf8(p.text, j);
            float cw = font->CalcTextSizeA(size, FLT_MAX, 0, p.text.c_str() + i, p.text.c_str() + j).x;
            if (x + w + cw > limit) break;
            shown += p.text.substr(i, j - i);
            w += cw;
            i = j;
        }
        if (dl) dl->AddText(font, size, ImVec2(at.x + x, at.y), colour, shown.c_str());
        x += w;
        break;
    }
    if (cut) {
        if (dl) dl->AddText(font, size, ImVec2(at.x + x, at.y), colour, ellipsis);
        x += ew;
    }
    return x;
}

std::string PlainText(const std::string& content, const Resolver& names) {
    std::string out;
    for (const auto& p : Parse(content)) {
        if (!out.empty()) out += ' ';
        if (p.code) {
            out += isAnsi(p.lang) ? StripAnsi(p.text, nullptr) : p.text;
            continue;
        }
        for (const auto& s : p.spans) {
            if (s.spoiler) {
                out += "\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88";  // ███
                continue;
            }
            switch (s.kind) {
            case Span::Kind::UserMention: out += "@" + names.user(s.id); break;
            case Span::Kind::RoleMention: out += "@" + names.role(s.id); break;
            case Span::Kind::ChannelMention: out += "#" + names.channel(s.id); break;
            case Span::Kind::CustomEmoji: out += ":" + s.text + ":"; break;
            case Span::Kind::Timestamp: out += names.time ? names.time(s.time, s.format) : ""; break;
            default: out += s.text;
            }
        }
    }
    return out;
}

}  // namespace who
