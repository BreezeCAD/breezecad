#include "drawing/DrawingPdf.h"

#include "drawing/Pdf.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <system_error>

namespace bz::drawing {

namespace {

// ГОСТ 2.303: the main line and a thin one; as on the screen.
constexpr double kThick = 0.5, kThin = 0.18;

/// The em of a label's font. As on the screen (DrawingEditor.cpp, DrawLabel):
/// the font's line (ascent + descent) is the label's height / 0.72.
double EmOf(const Label& label, const TrueTypeFont& font) {
    return label.height / 0.72 * font.unitsPerEm / std::max(1, font.ascent - font.descent);
}

std::string TitleOf(const Drawing& drawing, const SheetContent& content) {
    if (drawing.sheets.empty()) return {};
    const Sheet& first = drawing.sheets.front();
    Fields fields = content.fields ? content.fields(first) : Fields{};
    for (const auto& [key, value] : first.title.fields) fields[key] = value;   // the user's win
    const std::string designation = fields[field::kDesignation], name = fields[field::kName];
    return designation.empty() ? name : name.empty() ? designation : designation + " " + name;
}

} // namespace

std::vector<double> DashOf(LineStyle style) {
    switch (style) {
        case LineStyle::Solid:   return {};
        case LineStyle::Dashed:  return {4, 1.5};        // strokes 2–8 mm, gaps 1–2
        case LineStyle::DashDot: return {12, 3, 1, 3};   // strokes 5–30 mm, gaps 3–5
    }
    return {};
}

std::vector<std::uint8_t> DrawingPdf(const Drawing& drawing, const TrueTypeFont& font, std::string_view producer,
                                     const SheetContent& content) {
    PdfWriter pdf(font, "BreezeCADSans");
    for (const Page& page : PagesOf(drawing, content)) {
        const SheetLayout l = LayoutPage(drawing, page, content);
        pdf.BeginPage(l.width, l.height);
        for (const Stroke& s : l.strokes) pdf.Line(s.a, s.b, s.weight == LineWeight::Thick ? kThick : kThin);
        for (const Polyline& p : l.polylines) pdf.Polyline(p.points, p.weight == LineWeight::Thick ? kThick : kThin, DashOf(p.style));
        for (const Label& label : l.labels) {
            if (label.text.empty()) continue;
            const double em = EmOf(label, font), width = pdf.TextWidth(label.text, em);
            const double squeeze = label.maxWidth > 0 && width > label.maxWidth ? label.maxWidth / width : 1.0;
            Vec2 at = label.at;
            if (label.align == Align::Center) {
                const double a = label.rotation * kPi / 180.0, half = width * squeeze / 2;
                at = at - Vec2{std::cos(a), std::sin(a)} * half;
            }
            pdf.Text(label.text, at, em, label.rotation, squeeze);
        }
    }
    return pdf.Finish(TitleOf(drawing, content), producer);
}

Status ExportDrawingPdf(const Drawing& drawing, const std::filesystem::path& target, const TrueTypeFont& font,
                        std::string_view producer, const SheetContent& content) {
    const std::vector<std::uint8_t> bytes = DrawingPdf(drawing, font, producer, content);
    std::filesystem::path temp = target;
    temp += ".part";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!out) {
            std::error_code ec;
            std::filesystem::remove(temp, ec);
            return Error{"Не удалось записать PDF в выбранную папку"};
        }
    }
    std::error_code ec;
    std::filesystem::rename(temp, target, ec);
    if (ec) {
        std::filesystem::remove(temp, ec);
        return Error{"Не удалось записать PDF в выбранную папку"};
    }
    return Status::Ok();
}

} // namespace bz::drawing
