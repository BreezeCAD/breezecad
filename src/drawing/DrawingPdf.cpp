#include "drawing/DrawingPdf.h"

#include "drawing/Pdf.h"
#include "drawing/SheetLayout.h"

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

std::string TitleOf(const Drawing& drawing) {
    if (drawing.sheets.empty()) return {};
    const auto& fields = drawing.sheets.front().title.fields;
    const auto get = [&](const char* key) {
        const auto it = fields.find(key);
        return it == fields.end() ? std::string{} : it->second;
    };
    const std::string designation = get(field::kDesignation), name = get(field::kName);
    return designation.empty() ? name : name.empty() ? designation : designation + " " + name;
}

} // namespace

std::vector<std::uint8_t> DrawingPdf(const Drawing& drawing, const TrueTypeFont& font, std::string_view producer,
                                     const ViewImages* views) {
    PdfWriter pdf(font, "BreezeCADSans");
    const int count = static_cast<int>(drawing.sheets.size());
    for (int i = 0; i < count; ++i) {
        const SheetLayout l = LayoutSheet(drawing.sheets[static_cast<std::size_t>(i)], i + 1, count);
        pdf.BeginPage(l.width, l.height);
        for (const Stroke& s : l.strokes) pdf.Line(s.a, s.b, s.weight == LineWeight::Thick ? kThick : kThin);
        if (views)   // the model views: seen lines thick, hidden ones dashed thin (ViewLayout.h)
            for (const PlacedView& view : LayoutViews(drawing.sheets[static_cast<std::size_t>(i)], *views))
                for (const ViewPath& path : view.paths)
                    for (std::size_t k = 1; k < path.points.size(); ++k)
                        pdf.Line(path.points[k - 1], path.points[k], path.weight == LineWeight::Thick ? kThick : kThin);
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
    return pdf.Finish(TitleOf(drawing), producer);
}

Status ExportDrawingPdf(const Drawing& drawing, const std::filesystem::path& target, const TrueTypeFont& font,
                        std::string_view producer, const ViewImages* views) {
    const std::vector<std::uint8_t> bytes = DrawingPdf(drawing, font, producer, views);
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
