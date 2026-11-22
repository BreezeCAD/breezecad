#include "drawing/TrueType.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <optional>

namespace bz::drawing {

namespace {

/// Big-endian reads that never run past the end: a broken font is an error, not a crash.
struct Reader {
    std::span<const std::uint8_t> bytes;
    bool                          ok = true;

    std::uint32_t U(std::size_t at, int size) {
        if (at + static_cast<std::size_t>(size) > bytes.size()) {
            ok = false;
            return 0;
        }
        std::uint32_t v = 0;
        for (int i = 0; i < size; ++i) v = v << 8 | bytes[at + static_cast<std::size_t>(i)];
        return v;
    }
    std::uint16_t U16(std::size_t at) { return static_cast<std::uint16_t>(U(at, 2)); }
    std::int16_t  I16(std::size_t at) { return static_cast<std::int16_t>(U16(at)); }
    std::uint32_t U32(std::size_t at) { return U(at, 4); }
};

struct Table {
    std::size_t at = 0, size = 0;
};

std::optional<Table> FindTable(Reader& r, const char (&tag)[5]) {
    const std::uint16_t count = r.U16(4);
    for (std::size_t i = 0; i < count && r.ok; ++i) {
        const std::size_t rec = 12 + 16 * i;
        const std::uint32_t t = r.U32(rec);
        const std::uint32_t want = static_cast<std::uint32_t>(tag[0]) << 24 | static_cast<std::uint32_t>(tag[1]) << 16 |
                                   static_cast<std::uint32_t>(tag[2]) << 8 | static_cast<std::uint32_t>(tag[3]);
        if (t != want) continue;
        const Table table{r.U32(rec + 8), r.U32(rec + 12)};
        if (table.at + table.size > r.bytes.size()) return std::nullopt;
        return table;
    }
    return std::nullopt;
}

void ReadFormat4(Reader& r, std::size_t at, std::unordered_map<char32_t, std::uint16_t>& out) {
    const std::size_t segments = r.U16(at + 6) / 2;
    const std::size_t ends = at + 14, starts = ends + 2 * segments + 2, deltas = starts + 2 * segments,
                      offsets = deltas + 2 * segments;
    for (std::size_t s = 0; s < segments && r.ok; ++s) {
        const std::uint32_t end = r.U16(ends + 2 * s), start = r.U16(starts + 2 * s);
        const std::uint16_t delta = r.U16(deltas + 2 * s), offset = r.U16(offsets + 2 * s);
        for (std::uint32_t c = start; c <= end && c != 0xFFFF && r.ok; ++c) {
            std::uint32_t glyph;
            if (offset == 0) {
                glyph = (c + delta) & 0xFFFF;
            } else {
                glyph = r.U16(offsets + 2 * s + offset + 2 * (c - start));
                if (glyph != 0) glyph = (glyph + delta) & 0xFFFF;
            }
            if (glyph != 0) out.emplace(static_cast<char32_t>(c), static_cast<std::uint16_t>(glyph));
        }
    }
}

void ReadFormat12(Reader& r, std::size_t at, std::unordered_map<char32_t, std::uint16_t>& out) {
    const std::uint32_t groups = r.U32(at + 12);
    for (std::size_t g = 0; g < groups && r.ok; ++g) {
        const std::size_t rec = at + 16 + 12 * g;
        const std::uint32_t first = r.U32(rec), last = r.U32(rec + 4), glyph = r.U32(rec + 8);
        if (last < first || last - first > 0x10FFFF) {
            r.ok = false;
            return;
        }
        for (std::uint32_t c = first; c <= last; ++c)
            if (const std::uint32_t g2 = glyph + (c - first); g2 != 0 && g2 <= 0xFFFF)
                out.emplace(static_cast<char32_t>(c), static_cast<std::uint16_t>(g2));
    }
}

} // namespace

Expected<TrueTypeFont> TrueTypeFont::FromBytes(std::vector<std::uint8_t> bytes) {
    TrueTypeFont font;
    font.myBytes = std::move(bytes);
    Reader r{font.myBytes};
    const Error broken{"Файл шрифта повреждён или это не шрифт TrueType"};
    const std::uint32_t version = r.U32(0);
    if (!r.ok || (version != 0x00010000 && version != 0x74727565)) return broken;   // 1.0 or 'true'
    const auto head = FindTable(r, "head"), hhea = FindTable(r, "hhea"), hmtx = FindTable(r, "hmtx"),
               cmap = FindTable(r, "cmap");
    if (!head || !hhea || !hmtx || !cmap || !FindTable(r, "glyf")) return broken;

    font.unitsPerEm = r.U16(head->at + 18);
    for (int i = 0; i < 4; ++i) font.bbox[i] = r.I16(head->at + 36 + 2 * static_cast<std::size_t>(i));
    font.ascent = r.I16(hhea->at + 4);
    font.descent = r.I16(hhea->at + 6);
    const std::size_t metrics = r.U16(hhea->at + 34);
    for (std::size_t i = 0; i < metrics && r.ok; ++i) font.myAdvances.push_back(r.U16(hmtx->at + 4 * i));
    if (!r.ok || font.unitsPerEm == 0 || font.myAdvances.empty()) return broken;

    font.capHeight = font.unitsPerEm * 7 / 10;
    if (const auto os2 = FindTable(r, "OS/2")) {
        // fsType 2: «Restricted License embedding» — the font must not leave the computer.
        if ((r.U16(os2->at + 8) & 0x000F) == 0x0002) return Error{"Лицензия шрифта запрещает встраивать его в документы"};
        if (r.U16(os2->at) >= 2 && os2->size >= 90)
            if (const int cap = r.I16(os2->at + 88); cap > 0) font.capHeight = cap;
    }

    // The Unicode subtables: the full one (3,10 format 12) or the basic plane (format 4).
    const std::uint16_t subtables = r.U16(cmap->at + 2);
    std::optional<std::size_t> full, basic;
    for (std::size_t i = 0; i < subtables && r.ok; ++i) {
        const std::size_t rec = cmap->at + 4 + 8 * i;
        const std::uint16_t platform = r.U16(rec), encoding = r.U16(rec + 2);
        const std::size_t at = cmap->at + r.U32(rec + 4);
        const std::uint16_t format = r.U16(at);
        const bool unicode = platform == 0 || (platform == 3 && (encoding == 1 || encoding == 10));
        if (!unicode) continue;
        if (format == 12 && !full) full = at;
        if (format == 4 && !basic) basic = at;
    }
    if (full) ReadFormat12(r, *full, font.myGlyphs);
    else if (basic) ReadFormat4(r, *basic, font.myGlyphs);
    if (!r.ok || font.myGlyphs.empty()) return broken;
    return font;
}

Expected<TrueTypeFont> TrueTypeFont::FromFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return Error{"Не найден файл шрифта для PDF"};
    std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    return FromBytes(std::move(bytes));
}

std::uint16_t TrueTypeFont::Glyph(char32_t c) const {
    const auto it = myGlyphs.find(c);
    return it == myGlyphs.end() ? 0 : it->second;
}

int TrueTypeFont::Advance(std::uint16_t glyph) const {
    return myAdvances[std::min<std::size_t>(glyph, myAdvances.size() - 1)];
}

std::vector<char32_t> DecodeUtf8(std::string_view text) {
    std::vector<char32_t> out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        const auto b = static_cast<unsigned char>(text[i]);
        const int extra = b < 0x80 ? 0 : (b & 0xE0) == 0xC0 ? 1 : (b & 0xF0) == 0xE0 ? 2 : (b & 0xF8) == 0xF0 ? 3 : -1;
        char32_t c = extra == 0 ? b : extra == 1 ? b & 0x1F : extra == 2 ? b & 0x0F : b & 0x07;
        bool good = extra >= 0 && i + static_cast<std::size_t>(extra) < text.size();
        for (int k = 1; good && k <= extra; ++k) {
            const auto n = static_cast<unsigned char>(text[i + static_cast<std::size_t>(k)]);
            if ((n & 0xC0) != 0x80) good = false;
            else c = c << 6 | (n & 0x3F);
        }
        if (!good) {
            out.push_back(0xFFFD);
            ++i;
            continue;
        }
        out.push_back(c);
        i += static_cast<std::size_t>(extra) + 1;
    }
    return out;
}

} // namespace bz::drawing
