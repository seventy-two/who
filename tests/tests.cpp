// Tests for the parts that don't need a window: WhatsApp formatting.
#include <cstdio>

#include "waformat.h"

using namespace who;

static int failures = 0;
#define CHECK(c)                                                     \
    do {                                                             \
        if (!(c)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            failures++;                                              \
        }                                                            \
    } while (0)

int main() {
    auto p = ParseWhatsApp("hello *bold* and _it_ ~gone~ `code`");
    CHECK(p.size() == 1);
    bool bold = false, it = false, strike = false, code = false;
    for (auto& s : p[0].spans) {
        if (s.text == "bold" && (s.style & style::Bold)) bold = true;
        if (s.text == "it" && (s.style & style::Italic)) it = true;
        if (s.text == "gone" && (s.style & style::Strike)) strike = true;
        if (s.text == "code" && (s.style & style::Code)) code = true;
    }
    CHECK(bold && it && strike && code);
    // Not formatting: inside words, or with spaces inside the markers.
    p = ParseWhatsApp("2*3*4 and * not *");
    for (auto& s : p[0].spans) CHECK(!(s.style & style::Bold));
    p = ParseWhatsApp("see https://example.com/a?b=1.");
    bool link = false;
    for (auto& s : p[0].spans)
        if (s.kind == Span::Kind::Link && s.url == "https://example.com/a?b=1") link = true;
    CHECK(link);
    p = ParseWhatsApp("line one\n> quoted\n- item");
    CHECK(p.size() == 3 && p[1].quote && p[2].list == 1);
    p = ParseWhatsApp("```\nint x;\n```");
    CHECK(p.size() == 1 && p[0].code && p[0].text == "int x;");
    bool jumbo = false;
    ParseWhatsApp("\xF0\x9F\x98\x82\xF0\x9F\x98\x82", &jumbo);
    CHECK(jumbo);
    ParseWhatsApp("ha \xF0\x9F\x98\x82", &jumbo);
    CHECK(!jumbo);
    if (failures) {
        std::printf("%d failed\n", failures);
        return 1;
    }
    std::printf("tests passed\n");
    return 0;
}
