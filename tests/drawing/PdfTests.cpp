// The drawing as a PDF: the font read for it, the file's structure (objects
// where the cross-reference table says they are), pages a sheet, and the
// text findable (ToUnicode).
#include "drawing/DrawingPdf.h"
#include "drawing/Pdf.h"
#include "drawing/TrueType.h"

#include <doctest.h>
#include <miniz.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using namespace bz;
using namespace bz::drawing;

namespace {

const TrueTypeFont& Plex() {
    static const Expected<TrueTypeFont> font =
        TrueTypeFont::FromFile(fs::path(BZ_SOURCE_DIR) / "assets" / "fonts" / "IBMPlexSans-Regular.ttf");
    REQUIRE(font);
    return *font;
}

std::string Text(const std::vector<std::uint8_t>& bytes) { return {bytes.begin(), bytes.end()}; }

/// Every stream of the file, inflated.
std::string Streams(const std::string& pdf) {
    std::string out;
    for (std::size_t at = pdf.find(">>\nstream\n"); at != std::string::npos; at = pdf.find(">>\nstream\n", at)) {
        at += 10;
        const std::size_t end = pdf.find("\nendstream", at);
        REQUIRE(end != std::string::npos);
        std::string data(64u << 20, '\0');
        mz_ulong size = static_cast<mz_ulong>(data.size());
        if (mz_uncompress(reinterpret_cast<unsigned char*>(data.data()), &size, reinterpret_cast<const unsigned char*>(pdf.data() + at),
                          static_cast<mz_ulong>(end - at)) == MZ_OK)
            out += data.substr(0, size) + "\n";
        at = end;
    }
    return out;
}

} // namespace

TEST_CASE("TrueType: the interface font has Cyrillic and the sign of number") {
    const TrueTypeFont& f = Plex();
    CHECK(f.unitsPerEm > 0);
    CHECK(f.ascent > 0);
    CHECK(f.descent < 0);
    CHECK(f.capHeight > 0);
    for (char32_t c : {U'A', U'Ж', U'я', U'№', U'Ø'}) {
        CHECK(f.Glyph(c) != 0);
        CHECK(f.Advance(f.Glyph(c)) > 0);
    }
    CHECK(f.Glyph(U'\U0001F600') == 0);   // none: the «missing» glyph
}

TEST_CASE("TrueType: not a font is an error, not a crash") {
    CHECK_FALSE(TrueTypeFont::FromBytes({}));
    CHECK_FALSE(TrueTypeFont::FromBytes({0, 1, 0, 0, 0, 9, 0, 0}));
    std::vector<std::uint8_t> cut(Plex().Bytes().begin(), Plex().Bytes().begin() + 2000);
    CHECK_FALSE(TrueTypeFont::FromBytes(std::move(cut)));
    CHECK_FALSE(TrueTypeFont::FromFile("/nonexistent/font.ttf"));
}

TEST_CASE("UTF-8: Cyrillic, four bytes, and a broken byte") {
    CHECK(DecodeUtf8("Вал") == std::vector<char32_t>{U'В', U'а', U'л'});
    CHECK(DecodeUtf8("\xF0\x9F\x98\x80") == std::vector<char32_t>{U'\U0001F600'});
    CHECK(DecodeUtf8("a\xFF" "b") == std::vector<char32_t>{U'a', 0xFFFD, U'b'});
    CHECK(DecodeUtf8("\xD0") == std::vector<char32_t>{0xFFFD});   // cut in the middle
}

TEST_CASE("PDF: a page a sheet at full size, objects where the table says") {
    Drawing d;
    d.sheets.front().title.fields[field::kName] = "Вал";
    d.sheets.front().title.fields[field::kDesignation] = "БЗК.001";
    Sheet second;
    second.name = "Лист 2";
    second.size = SheetSize::A4;
    d.sheets.push_back(second);
    const std::string pdf = Text(DrawingPdf(d, Plex(), "BreezeCAD test"));

    CHECK(pdf.starts_with("%PDF-1.7\n"));
    CHECK(pdf.ends_with("%%EOF\n"));
    CHECK(pdf.find("/Count 2") != std::string::npos);
    CHECK(pdf.find("/MediaBox [0 0 1190.5512 841.8898]") != std::string::npos);   // A3 landscape, 420 × 297
    CHECK(pdf.find("/MediaBox [0 0 595.2756 841.8898]") != std::string::npos);    // A4 upright, 210 × 297
    CHECK(pdf.find("/FontFile2") != std::string::npos);

    // The cross-reference table: each offset is the start of its object.
    const std::size_t xref = std::stoul(pdf.substr(pdf.rfind("startxref\n") + 10));
    REQUIRE(pdf.compare(xref, 5, "xref\n") == 0);
    std::size_t line = pdf.find('\n', pdf.find('\n', xref + 5) + 1) + 1;   // after «0 N» and the free entry
    int objects = 0;
    while (pdf.compare(line, 7, "trailer") != 0) {
        const std::size_t at = std::stoul(pdf.substr(line, 10));
        ++objects;
        CHECK(pdf.compare(at, std::to_string(objects).size() + 6, std::to_string(objects) + " 0 obj") == 0);
        line += 20;
    }
    CHECK(objects > 10);

    // The text is there and can be found: the glyphs map back to «Вал».
    const std::string streams = Streams(pdf);
    CHECK(streams.find("/F1 ") != std::string::npos);
    CHECK(streams.find("<0412>") != std::string::npos);   // В
    CHECK(streams.find("<043B>") != std::string::npos);   // л
    CHECK(pdf.find("/Title <FEFF04110417041A") != std::string::npos);   // «БЗК…» in the properties
}

TEST_CASE("PDF: written through a temporary file; a bad folder is an error") {
    const fs::path dir = fs::temp_directory_path() / "bz_drawing_tests";
    fs::create_directories(dir);
    const fs::path file = dir / "Вал.pdf";
    REQUIRE(ExportDrawingPdf(Drawing{}, file, Plex(), "BreezeCAD test"));
    CHECK(fs::file_size(file) > 1000);
    CHECK_FALSE(fs::exists(fs::path(file) += ".part"));
    std::ifstream in(file, std::ios::binary);
    std::string head(8, '\0');
    in.read(head.data(), 8);
    CHECK(head == "%PDF-1.7");
    CHECK_FALSE(ExportDrawingPdf(Drawing{}, dir / "нет такой папки" / "x.pdf", Plex(), "BreezeCAD test"));
}

TEST_CASE("PDF: text width and squeeze") {
    PdfWriter pdf(Plex(), "Test");
    const double w = pdf.TextWidth("Разраб.", 5);
    CHECK(w > 5);
    CHECK(w < 30);
    CHECK(pdf.TextWidth("", 5) == 0);
    CHECK(pdf.TextWidth("ЖЖ", 5) == doctest::Approx(2 * pdf.TextWidth("Ж", 5)));
}
