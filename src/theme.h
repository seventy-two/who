// Colours, fonts and ImGui style, light and dark after Windows' app theme.
#pragma once

#include <string_view>

#include "imgui.h"

namespace who {

struct Palette {
    ImVec4 rail, sidebar, bg, text, muted, heading, accent, link, input, code, border, hover, selected;
    ImVec4 good, idle, dnd, bad, mentionBar, spoiler;
    ImVec4 band;  // every other author's messages sit on this
};

struct Fonts {
    ImFont* regular = nullptr;  // with the icons merged in
    ImFont* bold = nullptr;
    ImFont* italic = nullptr;
    ImFont* boldItalic = nullptr;
    ImFont* semibold = nullptr;  // headings, names; also with icons
    ImFont* mono = nullptr;
};

// Base sizes, before DPI scaling.
constexpr float kBody = 14.5f, kSmall = 12.5f, kH1 = 20, kH2 = 18, kH3 = 16, kJumbo = 40;

namespace icon {
inline constexpr const char* Settings = "\xEE\x9C\x93";  // U+E713
inline constexpr const char* Minimise = "\xEE\xA4\xA1";  // U+E921
inline constexpr const char* Maximise = "\xEE\xA4\xA2";  // U+E922
inline constexpr const char* Restore = "\xEE\xA4\xA3";  // U+E923
inline constexpr const char* Close = "\xEE\xA2\xBB";  // U+E8BB
inline constexpr const char* People = "\xEE\x9C\x96";  // U+E716
inline constexpr const char* Chat = "\xEE\xA2\xBD";  // U+E8BD
inline constexpr const char* Reply = "\xEE\xA3\x8A";  // U+E8CA
inline constexpr const char* Edit = "\xEE\x9C\x8F";  // U+E70F
inline constexpr const char* Delete = "\xEE\x9D\x8D";  // U+E74D
inline constexpr const char* Emoji = "\xEE\x9D\xAE";  // U+E76E
inline constexpr const char* Attach = "\xEE\x9C\xA3";  // U+E723
inline constexpr const char* Play = "\xEE\x9D\xA8";  // U+E768
inline constexpr const char* Pause = "\xEE\x9D\xA9";  // U+E769
inline constexpr const char* Volume = "\xEE\x9D\xA7";  // U+E767
inline constexpr const char* Mute = "\xEE\x9D\x8F";  // U+E74F
inline constexpr const char* Open = "\xEE\xA2\xA7";  // U+E8A7
inline constexpr const char* Dismiss = "\xEE\x9C\x91";  // U+E711
inline constexpr const char* List = "\xEE\xA3\xBD";  // U+E8FD BulletedList
inline constexpr const char* Down = "\xEE\x9C\x8D";  // U+E70D ChevronDown
inline constexpr const char* Bell = "\xEE\xAA\x8F";  // U+EA8F Ringer
inline constexpr const char* Speaker = "\xEE\xA6\x92";  // U+E992 Volume0, no waves
inline constexpr const char* Mic = "\xEE\x9C\xA0";  // U+E720 Microphone
inline constexpr const char* MicOff = "\xEE\xB1\x94";  // U+EC54
inline constexpr const char* Headphone = "\xEE\x9F\xB6";  // U+E7F6
inline constexpr const char* Video = "\xEE\x9C\x94";  // U+E714
inline constexpr const char* HangUp = "\xEE\x9D\xB8";  // U+E778
inline constexpr const char* Hash = "#";
}  // namespace icon

bool SystemUsesDarkTheme();
void ApplyTheme(bool dark, float scale);
const Palette& Colours();
// LoadFonts adds the fonts to ImGui's atlas, once at startup; with 1.92's
// dynamic atlas they're drawn at any size.
Fonts LoadFonts();

// NoteText looks for scripts the loaded fonts lack (Chinese, Japanese,
// Korean) in text about to be drawn. Their fonts are big, so they're only
// loaded once something needs them.
void NoteText(std::string_view text);
// LoadNeededFonts merges in the fonts NoteText found wanting. Call it
// between frames; it returns true if it added any, so layouts measured
// without them should be redone.
bool LoadNeededFonts(const Fonts& fonts);

ImU32 U32(const ImVec4& c, float alpha = 1.0f);
ImVec4 Hex(unsigned rgb, float a = 1.0f);

}  // namespace who
