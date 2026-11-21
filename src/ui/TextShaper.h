#pragma once
// The text hook of the interface (ImGui::GImTextShaper): Arabic text is put
// into visual order before it is measured or drawn, wrapped by words when the
// caller wraps, with the lines pressed to the right edge of the block.

struct ImFont;

namespace bz::ui {

/// Matches GImTextShaper. Returns nullptr for text without Arabic; otherwise
/// the text to use instead (valid until the next call). When the text had to
/// be wrapped (`*wrapWidth` > 0) the lines are separated by '\n' and
/// `*wrapWidth` becomes 0.
const char* ShapeText(ImFont* font, float fontSize, const char* text, const char* textEnd, float* wrapWidth,
                      const char** shapedEnd);

/// Forget what was shaped (the fonts were built again).
void ResetTextShaper();

} // namespace bz::ui
