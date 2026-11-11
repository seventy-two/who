// Laying out and drawing a message's parsed markdown: words wrapped to a
// width, with emoji as images, mentions as pills, code in boxes.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "imgui.h"
#include "markdown.h"
#include "theme.h"

namespace who {

struct Piece {
    enum class Kind { Text, Image, Box, Bar } kind = Kind::Text;
    ImVec2 pos, size;  // relative to the layout's top left
    std::string text;
    ImFont* font = nullptr;
    float fontSize = 0;
    ImU32 colour = 0;
    uint16_t style = 0;
    std::string image;    // an image's URL
    std::string fallback;  // an image's text, until it loads
    std::string link;     // a link's URL
    uint64_t channel = 0;  // a channel mention's channel
    int spoiler = 0;
    bool pill = false;  // a mention's background
    ImU32 back = 0;     // an ```ansi run's background, 0 for none
    bool thicken = false;  // bold where there's no bold font (mono)
};

struct Layout {
    std::vector<Piece> pieces;
    float width = 0, height = 0;
};

// Resolver names what spans refer to, from the store.
struct Resolver {
    std::function<std::string(uint64_t)> user, role, channel;
    std::function<ImU32(uint64_t)> roleColour;  // 0 for none
    std::function<std::string(int64_t, char)> time;
};

// LayOut wraps paragraphs to a width. scale is the DPI scale; muted draws
// everything in the muted colour (pending messages, embed footers).
Layout LayOut(const std::vector<Para>& paras, float width, const Fonts& fonts, const Resolver& names, float scale,
              bool jumbo = false, float bodySize = kBody, bool muted = false);

// Hit is what's under the mouse in a drawn layout.
struct Hit {
    const Piece* piece = nullptr;
};

// ImageFor gives the texture for an image URL, or 0 while it loads.
using ImageFor = std::function<ImTextureID(const std::string& url)>;

// Draw draws a layout at a screen position. revealed lists spoilers shown;
// it returns the piece under the mouse, if any.
Hit Draw(const Layout& l, ImVec2 at, const ImageFor& images, const std::vector<int>& revealed);

// Label draws one line of plain text with any emoji as images, cut short
// with an ellipsis past maxWidth (0 for no limit). It returns the width
// drawn. With dl null it only measures.
float Label(ImDrawList* dl, ImFont* font, float size, ImVec2 at, ImU32 colour, const std::string& text, float maxWidth,
            const ImageFor& images);

// PlainText turns content into one line of plain text, mentions named, for
// notifications and reply previews.
std::string PlainText(const std::string& content, const Resolver& names);

}  // namespace who
