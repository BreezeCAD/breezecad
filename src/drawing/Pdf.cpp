#include "drawing/Pdf.h"

#include <miniz.h>

#include <algorithm>
#include <cmath>
#include <format>

namespace bz::drawing {

namespace {

constexpr double kPointsPerMm = 72.0 / 25.4;

/// A number as PDF wants it: no exponent, no needless zeros, «.» always.
std::string Num(double v) {
    if (std::abs(v) < 5e-5) return "0";
    std::string s = std::format("{:.4f}", v);
    while (s.back() == '0') s.pop_back();
    if (s.back() == '.') s.pop_back();
    return s;
}

void AppendHex16(std::string& out, unsigned v) { out += std::format("{:04X}", v & 0xFFFF); }

/// UTF-16BE of a character, as hex: a surrogate pair above the basic plane.
void AppendUtf16(std::string& out, char32_t c) {
    if (c > 0xFFFF) {
        c -= 0x10000;
        AppendHex16(out, 0xD800 + static_cast<unsigned>(c >> 10));
        AppendHex16(out, 0xDC00 + static_cast<unsigned>(c & 0x3FF));
    } else {
        AppendHex16(out, static_cast<unsigned>(c));
    }
}

/// A text string of the document's properties: UTF-16BE with its mark.
std::string TextString(std::string_view utf8) {
    std::string out = "<FEFF";
    for (char32_t c : DecodeUtf8(utf8)) AppendUtf16(out, c);
    return out + ">";
}

std::string Deflate(std::string_view data) {
    mz_ulong size = mz_compressBound(static_cast<mz_ulong>(data.size()));
    std::string out(size, '\0');
    if (mz_compress2(reinterpret_cast<unsigned char*>(out.data()), &size, reinterpret_cast<const unsigned char*>(data.data()),
                     static_cast<mz_ulong>(data.size()), MZ_BEST_COMPRESSION) != MZ_OK)
        return {};
    out.resize(size);
    return out;
}

/// The file's objects, numbered from 1 in the order they are added.
class Objects {
public:
    int Reserve() {
        myBodies.emplace_back();
        return static_cast<int>(myBodies.size());
    }
    void Set(int id, std::string body) { myBodies[static_cast<std::size_t>(id - 1)] = std::move(body); }
    int Add(std::string body) {
        const int id = Reserve();
        Set(id, std::move(body));
        return id;
    }
    /// A compressed stream; `extra` — more keys of its dictionary.
    int AddStream(std::string_view data, const std::string& extra = {}) {
        std::string packed = Deflate(data);
        const bool flate = !packed.empty();
        if (!flate) packed = std::string(data);
        return Add(std::format("<< /Length {}{}{} >>\nstream\n", packed.size(), flate ? " /Filter /FlateDecode" : "", extra) +
                   packed + "\nendstream");
    }

    std::vector<std::uint8_t> Write(int root, int info) const {
        // The binary comment line tells transfer programs the file is not text.
        std::string out = "%PDF-1.7\n%";
        for (const int c : {0xE2, 0xE3, 0xCF, 0xD3}) out += static_cast<char>(c);
        out += '\n';
        std::vector<std::size_t> offsets;
        for (std::size_t i = 0; i < myBodies.size(); ++i) {
            offsets.push_back(out.size());
            out += std::format("{} 0 obj\n", i + 1) + myBodies[i] + "\nendobj\n";
        }
        const std::size_t xref = out.size();
        out += std::format("xref\n0 {}\n0000000000 65535 f \n", myBodies.size() + 1);
        for (std::size_t at : offsets) out += std::format("{:010} 00000 n \n", at);
        out += std::format("trailer\n<< /Size {} /Root {} 0 R /Info {} 0 R >>\nstartxref\n{}\n%%EOF\n", myBodies.size() + 1, root,
                           info, xref);
        return {out.begin(), out.end()};
    }

private:
    std::vector<std::string> myBodies;
};

} // namespace

PdfWriter::PdfWriter(const TrueTypeFont& font, std::string fontName) : myFont(font), myFontName(std::move(fontName)) {}

void PdfWriter::BeginPage(double widthMm, double heightMm) {
    Page page{widthMm * kPointsPerMm, heightMm * kPointsPerMm, {}};
    // Millimetres from here on; black ink, square line ends so the frame's corners close.
    page.content = std::format("{0} 0 0 {0} 0 0 cm\n0 g 0 G 2 J 0 j\n", Num(kPointsPerMm));
    myPages.push_back(std::move(page));
}

void PdfWriter::Line(Vec2 a, Vec2 b, double widthMm) {
    if (myPages.empty()) return;
    myPages.back().content +=
        std::format("{} w {} {} m {} {} l S\n", Num(widthMm), Num(a.x), Num(a.y), Num(b.x), Num(b.y));
}

void PdfWriter::Text(std::string_view utf8, Vec2 at, double emMm, double rotation, double squeeze) {
    if (myPages.empty() || utf8.empty() || emMm <= 0) return;
    std::string glyphs;
    for (char32_t c : DecodeUtf8(utf8)) {
        const std::uint16_t g = myFont.Glyph(c);
        myUsed.emplace(g, c);
        AppendHex16(glyphs, g);
    }
    const double a = rotation * kPi / 180.0, cs = std::cos(a), sn = std::sin(a);
    myPages.back().content += std::format("BT /F1 {} Tf {} Tz {} {} {} {} {} {} Tm <{}> Tj ET\n", Num(emMm), Num(squeeze * 100),
                                          Num(cs), Num(sn), Num(-sn), Num(cs), Num(at.x), Num(at.y), glyphs);
}

double PdfWriter::TextWidth(std::string_view utf8, double emMm) const {
    double units = 0;
    for (char32_t c : DecodeUtf8(utf8)) units += myFont.Advance(myFont.Glyph(c));
    return units * emMm / myFont.unitsPerEm;
}

std::vector<std::uint8_t> PdfWriter::Finish(std::string_view title, std::string_view producer) {
    Objects o;
    const int catalog = o.Reserve(), pages = o.Reserve();
    const int info = o.Add(std::format("<< /Title {} /Producer {} /Creator {} >>", TextString(title), TextString(producer),
                                       TextString(producer)));

    // The font: Type0 over a TrueType CID font, glyph numbers as they are.
    const double scale = 1000.0 / myFont.unitsPerEm;
    const auto em = [&](int v) { return Num(std::round(v * scale)); };
    const std::span<const std::uint8_t> bytes = myFont.Bytes();
    const int file = o.AddStream(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()),
                                 std::format(" /Length1 {}", bytes.size()));
    const int descriptor = o.Add(std::format(
        "<< /Type /FontDescriptor /FontName /{} /Flags 32 /FontBBox [{} {} {} {}] /ItalicAngle 0 /Ascent {} /Descent {} "
        "/CapHeight {} /StemV 80 /FontFile2 {} 0 R >>",
        myFontName, em(myFont.bbox[0]), em(myFont.bbox[1]), em(myFont.bbox[2]), em(myFont.bbox[3]), em(myFont.ascent),
        em(myFont.descent), em(myFont.capHeight), file));
    std::string widths;
    for (const auto& [glyph, c] : myUsed) widths += std::format("{} [{}] ", glyph, em(myFont.Advance(glyph)));
    const int cid = o.Add(std::format("<< /Type /Font /Subtype /CIDFontType2 /BaseFont /{} /CIDSystemInfo << /Registry (Adobe) "
                                      "/Ordering (Identity) /Supplement 0 >> /FontDescriptor {} 0 R /W [{}] /CIDToGIDMap /Identity >>",
                                      myFontName, descriptor, widths));
    std::string cmap = "/CIDInit /ProcSet findresource begin\n12 dict begin\nbegincmap\n"
                       "/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def\n"
                       "/CMapName /Adobe-Identity-UCS def\n/CMapType 2 def\n"
                       "1 begincodespacerange\n<0000> <FFFF>\nendcodespacerange\n";
    std::vector<std::pair<std::uint16_t, char32_t>> mapped;
    for (const auto& [glyph, c] : myUsed)
        if (glyph != 0) mapped.emplace_back(glyph, c);
    for (std::size_t i = 0; i < mapped.size(); i += 100) {   // at most 100 a block
        const std::size_t n = std::min<std::size_t>(100, mapped.size() - i);
        cmap += std::format("{} beginbfchar\n", n);
        for (std::size_t k = i; k < i + n; ++k) {
            cmap += "<";
            AppendHex16(cmap, mapped[k].first);
            cmap += "> <";
            AppendUtf16(cmap, mapped[k].second);
            cmap += ">\n";
        }
        cmap += "endbfchar\n";
    }
    cmap += "endcmap\nCMapName currentdict /CMap defineresource pop\nend\nend\n";
    const int unicode = o.AddStream(cmap);
    const int font = o.Add(std::format("<< /Type /Font /Subtype /Type0 /BaseFont /{} /Encoding /Identity-H /DescendantFonts [{} 0 R] "
                                       "/ToUnicode {} 0 R >>",
                                       myFontName, cid, unicode));

    std::string kids;
    for (const Page& p : myPages) {
        const int content = o.AddStream(p.content);
        const int page = o.Add(std::format("<< /Type /Page /Parent {} 0 R /MediaBox [0 0 {} {}] /Resources << /Font << /F1 {} 0 R >> >> "
                                           "/Contents {} 0 R >>",
                                           pages, Num(p.width), Num(p.height), font, content));
        kids += std::format("{} 0 R ", page);
    }
    o.Set(pages, std::format("<< /Type /Pages /Kids [{}] /Count {} >>", kids, myPages.size()));
    o.Set(catalog, std::format("<< /Type /Catalog /Pages {} 0 R >>", pages));
    return o.Write(catalog, info);
}

} // namespace bz::drawing
