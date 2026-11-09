#pragma once
// What a PDF needs to know of a TrueType font (.ttf) to show text with it:
// the glyph of a character, its advance and the font's vertical metrics.
// Only the tables a PDF needs are read (head, hhea, hmtx, cmap, OS/2); the
// bytes go into the PDF as they are (FontFile2).

#include "core/Expected.h"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace bz::drawing {

class TrueTypeFont {
public:
    /// Reads a font from its bytes; a font whose licence forbids embedding
    /// (OS/2 fsType 2) is an error too.
    static Expected<TrueTypeFont> FromBytes(std::vector<std::uint8_t> bytes);
    static Expected<TrueTypeFont> FromFile(const std::filesystem::path& path);

    /// The glyph of a Unicode character; 0 (the font's «missing» glyph) if none.
    std::uint16_t Glyph(char32_t c) const;
    /// The glyph's advance, in font units.
    int Advance(std::uint16_t glyph) const;

    int unitsPerEm = 1000;
    int ascent = 0, descent = 0;   ///< hhea: above and below the base line (descent < 0)
    int capHeight = 0;             ///< OS/2; 70 % of the em when the font has none
    int bbox[4] = {0, 0, 0, 0};    ///< head: xMin, yMin, xMax, yMax
    std::span<const std::uint8_t> Bytes() const { return myBytes; }

private:
    std::vector<std::uint8_t>                     myBytes;
    std::vector<std::uint16_t>                    myAdvances;   ///< hmtx; the last one repeats
    std::unordered_map<char32_t, std::uint16_t>   myGlyphs;     ///< cmap
};

/// Unicode characters of UTF-8 text; a broken byte becomes U+FFFD.
std::vector<char32_t> DecodeUtf8(std::string_view text);

} // namespace bz::drawing
