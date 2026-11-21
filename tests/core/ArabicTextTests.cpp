#include "core/ArabicText.h"

#include <doctest.h>

#include <string>

using bz::arabic::HasArabic;
using bz::arabic::ToVisual;

namespace {

/// UTF-8 of code points: the tests write Arabic as numbers, not as letters in the source.
std::string U(const std::u32string& text) {
    std::string out;
    for (const char32_t c : text) {
        if (c < 0x80) {
            out.push_back(static_cast<char>(c));
        } else if (c < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (c >> 6)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xE0 | (c >> 12)));
            out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        }
    }
    return out;
}

} // namespace

TEST_CASE("text without Arabic is left alone") {
    CHECK(ToVisual("Hello 123") == "Hello 123");
    CHECK(ToVisual(U(U"\u041F\u0440\u0438\u0432\u0435\u0442")) == U(U"\u041F\u0440\u0438\u0432\u0435\u0442"));
    CHECK_FALSE(HasArabic("Done"));
    CHECK(HasArabic(U(U"\u0641")));
}

TEST_CASE("letters take their forms and the word is reversed") {
    // fa, ta, ha: initial, medial, final; drawn left to right the final form comes first.
    CHECK(ToVisual(U(U"\u0641\u062A\u062D")) == U(U"\uFEA2\uFE98\uFED3"));
    // A letter that does not join to the next one breaks the word.
    CHECK(ToVisual(U(U"\u062F\u0628")) == U(U"\uFE8F\uFEA9"));
}

TEST_CASE("lam and alef make one glyph") {
    CHECK(ToVisual(U(U"\u0644\u0627")) == U(U"\uFEFB"));
    CHECK(ToVisual(U(U"\u0628\u0644\u0627")) == U(U"\uFEFC\uFE91"));
}

TEST_CASE("marks are dropped") {
    CHECK(ToVisual(U(U"\u0628\u064F")) == U(U"\uFE8F"));
}

TEST_CASE("numbers and Latin words keep their own direction inside right-to-left text") {
    CHECK(ToVisual(U(U"\u0645 123")) == U(U"123 \uFEE1"));
    CHECK(ToVisual(U(U"\u0645 abc \u0645")) == U(U"\uFEE1 abc \uFEE1"));
    // The unit sticks to its number: "180°" stays "180°".
    CHECK(ToVisual(U(U"\u0645 180\u00B0")) == U(U"180\u00B0 \uFEE1"));
    // Two words, the first one on the right.
    CHECK(ToVisual(U(U"\u0645 \u0645")) == U(U"\uFEE1 \uFEE1"));
}

TEST_CASE("brackets are mirrored") {
    CHECK(ToVisual(U(U"(\u0645)")) == U(U"(\uFEE1)"));
    CHECK(ToVisual(U(U"\u00AB\u0645\u00BB")) == U(U"\u00AB\uFEE1\u00BB"));
}
