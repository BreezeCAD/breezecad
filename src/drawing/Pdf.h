#pragma once
// A small PDF 1.7 writer for drawings: pages in millimetres, lines and text
// in one embedded TrueType font (Identity-H, so any script it covers, with
// a ToUnicode map so the text can be found and copied). Vector only, no
// images; streams are compressed. Nothing leaves the computer.

#include "core/Math.h"
#include "drawing/TrueType.h"

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace bz::drawing {

class PdfWriter {
public:
    /// `fontName` — the PostScript name for the PDF (letters and digits).
    PdfWriter(const TrueTypeFont& font, std::string fontName);

    /// Starts a page; millimetres from its lower left corner, y up.
    void BeginPage(double widthMm, double heightMm);
    void Line(Vec2 a, Vec2 b, double widthMm);
    /// Points joined in order; `dash` — lengths of strokes and gaps, mm (empty: solid).
    void Polyline(const std::vector<Vec2>& points, double widthMm, const std::vector<double>& dash = {});
    /// A filled shape (an arrow): its outline, closed.
    void Fill(const std::vector<Vec2>& points);
    /// Text with its base line from `at`; `emMm` — the font's em, `rotation`
    /// in degrees counter-clockwise, `squeeze` — horizontal scale (1 as is).
    void Text(std::string_view utf8, Vec2 at, double emMm, double rotation = 0, double squeeze = 1);
    /// The text's advance in mm at the em `emMm`.
    double TextWidth(std::string_view utf8, double emMm) const;

    /// The whole file. `title` goes into the document's properties.
    std::vector<std::uint8_t> Finish(std::string_view title, std::string_view producer);

private:
    struct Page {
        double      width = 0, height = 0;   ///< points
        std::string content;
    };
    const TrueTypeFont&                     myFont;
    std::string                             myFontName;
    std::vector<Page>                       myPages;
    std::map<std::uint16_t, char32_t>       myUsed;   ///< glyph → its character, for ToUnicode
};

} // namespace bz::drawing
