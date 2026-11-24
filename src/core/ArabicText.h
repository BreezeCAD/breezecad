#pragma once
// Arabic text for a renderer that draws glyphs left to right and knows
// nothing about scripts (Dear ImGui): letters are joined into their
// contextual forms and the line is put into visual order, so that drawing it
// as it is shows the text right.
//
//   ToVisual("فتح ملف")   ->  the same text as presentation forms, reversed
//
// A line without Arabic letters comes back as it is. Not a full Unicode
// bidirectional algorithm: one paragraph per line, base direction right to
// left, no embeddings; numbers, Latin words and punctuation are placed the
// way the algorithm places them in ordinary interface text.

#include <string>
#include <string_view>

namespace bz::arabic {

/// True when the UTF-8 text has an Arabic letter (or an Arabic presentation form).
bool HasArabic(std::string_view utf8);

/// One line of logical UTF-8 text in visual order, UTF-8. Marks (harakat)
/// are dropped: without glyph positioning they would sit in the wrong place.
std::string ToVisual(std::string_view line);

} // namespace bz::arabic
