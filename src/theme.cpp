#include "theme.h"

#include <windows.h>

#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace who {

ImVec4 Hex(unsigned rgb, float a) {
    return ImVec4(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, a);
}

ImU32 U32(const ImVec4& c, float alpha) { return ImGui::ColorConvertFloat4ToU32(ImVec4(c.x, c.y, c.z, c.w * alpha)); }

namespace {

// kitchensink's dark palette: near-black blue-greys, one blue accent.
// Order: rail, sidebar, bg, text, muted, heading, accent, link, input,
// code, border, hover, selected, good, idle, dnd, bad,
// mentionBar, spoiler, band.
const Palette kDark = {
    Hex(0x101216), Hex(0x1A1D23), Hex(0x15171C), Hex(0xE6E8EC), Hex(0x8C929D), Hex(0xF4F5F7), Hex(0x5AA8FF),
    Hex(0x5AA8FF), Hex(0x2A2E37), Hex(0x1E2128), Hex(0x2B2F38), Hex(0x262A33), Hex(0x2B2F38), Hex(0x3FB950),
    Hex(0xD9A23A), Hex(0xF8645A), Hex(0xF8645A), Hex(0xA78BFA, 0.6f), Hex(0x0B0C0F), Hex(0x1A1D23),
};

Palette current = kDark;


std::string fontPath(const char* file) {
    char dir[MAX_PATH];
    GetWindowsDirectoryA(dir, MAX_PATH);
    return std::string(dir) + "\\Fonts\\" + file;
}

bool exists(const std::string& path) { return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

// fontData reads a font file once, so fonts merged into several faces
// (icons, symbols) share one copy in memory.
std::vector<char>* fontData(const std::string& path) {
    static std::map<std::string, std::vector<char>> files;
    auto it = files.find(path);
    if (it != files.end()) return it->second.empty() ? nullptr : &it->second;
    std::vector<char>& data = files[path];
    std::ifstream f(path, std::ios::binary);
    if (f) data.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return data.empty() ? nullptr : &data;
}

ImFont* addFont(const std::string& path, ImFontConfig cfg, const ImWchar* ranges = nullptr) {
    std::vector<char>* data = fontData(path);
    if (!data) return nullptr;
    cfg.FontDataOwnedByAtlas = false;
    return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(data->data(), int(data->size()), kBody, &cfg, ranges);
}

// Scripts that need a font loading on demand: what's wanted and what's in.
enum Script { Han = 1, Hangul = 2 };
int wanted = 0, loaded = 0;

}  // namespace

void NoteText(std::string_view text) {
    if ((wanted | loaded) == (Han | Hangul)) return;
    for (size_t i = 0; i < text.size(); i++) {
        unsigned char b = (unsigned char)text[i];
        if (b < 0xE1 || b > 0xEF || i + 2 >= text.size()) continue;  // CJK is all 3-byte UTF-8
        uint32_t cp = ((b & 0x0F) << 12) | ((text[i + 1] & 0x3F) << 6) | (text[i + 2] & 0x3F);
        if ((cp >= 0x1100 && cp <= 0x11FF) || (cp >= 0x3130 && cp <= 0x318F) || (cp >= 0xAC00 && cp <= 0xD7AF))
            wanted |= Hangul;
        else if ((cp >= 0x2E80 && cp <= 0x9FFF) || (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0xFF00 && cp <= 0xFFEF))
            wanted |= Han;
        i += 2;
    }
}

bool LoadNeededFonts(const Fonts& f) {
    int add = wanted & ~loaded;
    if (!add) return false;
    // Microsoft YaHei for Chinese (and Japanese, kana included); Malgun
    // Gothic for Korean. Merged into every face, sharing one copy.
    auto merge = [&](const char* file) {
        std::string path = fontPath(file);
        for (ImFont* dst : {f.regular, f.bold, f.italic, f.boldItalic, f.semibold, f.mono}) {
            ImFontConfig cfg;
            cfg.MergeMode = true;
            cfg.DstFont = dst;
            addFont(path, cfg);
        }
    };
    if (add & Han) merge("msyh.ttc");
    if (add & Hangul) merge("malgun.ttf");
    loaded |= add;
    return true;
}

// Always dark, whatever Windows uses: Who has one theme.
bool SystemUsesDarkTheme() { return true; }

const Palette& Colours() { return current; }

void ApplyTheme(bool dark, float scale) {
    (void)dark;
    current = kDark;
    const Palette& p = current;
    ImGuiStyle& s = ImGui::GetStyle();
    s = ImGuiStyle();
    s.WindowPadding = ImVec2(0, 0);
    s.FramePadding = ImVec2(10, 7);
    s.ItemSpacing = ImVec2(8, 4);
    s.ItemInnerSpacing = ImVec2(6, 4);
    s.ScrollbarSize = 8;
    s.TabRounding = 0;
    s.WindowRounding = 0;
    s.ChildRounding = 0;
    s.FrameRounding = 0;
    s.PopupRounding = 0;
    s.ScrollbarRounding = 0;
    s.GrabRounding = 0;
    s.WindowBorderSize = 0;
    s.ChildBorderSize = 0;
    s.PopupBorderSize = 1;
    s.FrameBorderSize = 0;
    s.FontSizeBase = kBody;
    s.ScaleAllSizes(scale);
    s.FontScaleDpi = scale;

    ImVec4* c = s.Colors;
    ImVec4 none(0, 0, 0, 0);
    c[ImGuiCol_Text] = p.text;
    c[ImGuiCol_TextDisabled] = p.muted;
    c[ImGuiCol_WindowBg] = p.bg;
    c[ImGuiCol_ChildBg] = none;
    c[ImGuiCol_PopupBg] = p.sidebar;
    c[ImGuiCol_Border] = p.border;
    c[ImGuiCol_BorderShadow] = none;
    c[ImGuiCol_FrameBg] = p.input;
    c[ImGuiCol_FrameBgHovered] = p.input;
    c[ImGuiCol_FrameBgActive] = p.input;
    c[ImGuiCol_ScrollbarBg] = none;
    c[ImGuiCol_ScrollbarGrab] = p.rail;
    c[ImGuiCol_ScrollbarGrabHovered] = p.muted;
    c[ImGuiCol_ScrollbarGrabActive] = p.muted;
    c[ImGuiCol_CheckMark] = p.accent;
    c[ImGuiCol_SliderGrab] = p.accent;
    c[ImGuiCol_SliderGrabActive] = p.accent;
    c[ImGuiCol_Button] = p.selected;
    c[ImGuiCol_ButtonHovered] = p.hover;
    c[ImGuiCol_ButtonActive] = p.border;
    c[ImGuiCol_Header] = p.selected;
    c[ImGuiCol_HeaderHovered] = p.hover;
    c[ImGuiCol_HeaderActive] = p.selected;
    c[ImGuiCol_Separator] = p.border;
    c[ImGuiCol_TextSelectedBg] = ImVec4(p.accent.x, p.accent.y, p.accent.z, 0.4f);
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, dark ? 0.6f : 0.35f);
    c[ImGuiCol_NavCursor] = ImVec4(0, 0, 0, 0);  // no keyboard focus outline
}

Fonts LoadFonts() {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    std::string iconFont = fontPath("SegoeIcons.ttf");
    if (!exists(iconFont)) iconFont = fontPath("segmdl2.ttf");
    // Symbols Segoe UI lacks (arrows, maths, box drawing) come from Segoe
    // UI Symbol. CJK fonts are left out: each is tens of MB in memory.
    std::string symbols = fontPath("seguisym.ttf");

    auto add = [&](const char* file, bool icons) -> ImFont* {
        ImFont* f = addFont(fontPath(file), ImFontConfig());
        if (!f) return io.Fonts->AddFontDefault();
        if (icons) {
            ImFontConfig merge;
            merge.MergeMode = true;
            merge.GlyphOffset = ImVec2(0, 1);
            static const ImWchar range[] = {0xE700, 0xF8FF, 0};
            addFont(iconFont, merge, range);
        }
        ImFontConfig merge;
        merge.MergeMode = true;
        addFont(symbols, merge);
        return f;
    };
    Fonts f;
    f.regular = add("segoeui.ttf", true);
    f.bold = add("segoeuib.ttf", false);
    f.italic = add("segoeuii.ttf", false);
    f.boldItalic = add("segoeuiz.ttf", false);
    f.semibold = add("seguisb.ttf", true);
    f.mono = add("consola.ttf", false);
    io.FontDefault = f.regular;
    return f;
}

}  // namespace who
