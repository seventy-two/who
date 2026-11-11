// WhatsApp's text formatting (*bold*, _italic_, ~strike~, `code`,
// ```monospace```, > quotes, - lists), plus links and emoji, parsed into
// the paragraphs richtext lays out.
#pragma once

#include <string_view>
#include <vector>

#include "markdown.h"

namespace who {

// ParseWhatsApp reads a message's text; jumbo says it's just one to three
// emoji, drawn large.
std::vector<Para> ParseWhatsApp(std::string_view text, bool* jumbo = nullptr);

}  // namespace who
