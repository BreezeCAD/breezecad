#include "drawing/DrawingDxf.h"

#include "drawing/DrawingPdf.h"
#include "sheetmetal/DxfDrawing.h"

#include <cmath>
#include <fstream>
#include <system_error>

namespace bz::drawing {

namespace {

constexpr const char* kThick  = "THICK";
constexpr const char* kThin   = "THIN";
constexpr const char* kHidden = "HIDDEN";
constexpr const char* kAxis   = "AXIS";
constexpr const char* kText   = "TEXT";

const char* LayerOf(LineWeight weight, LineStyle style) {
    if (style == LineStyle::Dashed) return kHidden;
    if (style == LineStyle::DashDot) return kAxis;
    return weight == LineWeight::Thick ? kThick : kThin;
}

/// A polyline with its dashes (ГОСТ 2.303, DashOf): the strokes as pieces.
void AddDashed(dxf::Drawing& out, const std::vector<Vec2>& points, const std::vector<double>& dash, const char* layer, Vec2 shift) {
    if (points.size() < 2) return;
    if (dash.empty()) {
        dxf::Entity e;
        e.kind = dxf::Kind::Polyline;
        e.layer = layer;
        for (Vec2 p : points) e.points.push_back(p + shift);
        e.bulges.assign(e.points.size(), 0.0);
        out.entities.push_back(std::move(e));
        return;
    }
    std::size_t k = 0;
    double left = dash[0];
    std::vector<Vec2> piece{points[0] + shift};
    const auto flush = [&] {
        if (k % 2 == 0 && piece.size() >= 2) {
            dxf::Entity e;
            e.kind = dxf::Kind::Polyline;
            e.layer = layer;
            e.points = piece;
            e.bulges.assign(piece.size(), 0.0);
            out.entities.push_back(std::move(e));
        }
    };
    for (std::size_t i = 1; i < points.size(); ++i) {
        const Vec2 a = points[i - 1], b = points[i];
        const double len = (b - a).Length();
        if (len <= 0) continue;
        const Vec2 dir = (b - a) / len;
        double at = 0;
        while (at < len) {
            const double step = std::min(left, len - at);
            at += step;
            left -= step;
            piece.push_back(a + dir * at + shift);
            if (left <= 1e-9) {
                flush();
                k = (k + 1) % dash.size();
                left = dash[k];
                piece = {a + dir * at + shift};
            }
        }
    }
    flush();
}

/// About the width of a text: the receiving program draws it in its own font.
double TextWidth(const std::string& text, double height) {
    std::size_t chars = 0;
    for (const char c : text)
        if ((static_cast<unsigned char>(c) & 0xC0U) != 0x80U) ++chars;
    return static_cast<double>(chars) * height * 0.8;
}

} // namespace

std::string DrawingDxf(const Drawing& drawing, const SheetContent& content) {
    dxf::Drawing out;
    out.EnsureLayer(kThick, 7);
    out.EnsureLayer(kThin, 3);
    out.EnsureLayer(kHidden, 5);
    out.EnsureLayer(kAxis, 1);
    out.EnsureLayer(kText, 7);
    double x = 0;
    for (const Page& page : PagesOf(drawing, content)) {
        const SheetLayout l = LayoutPage(drawing, page, content);
        const Vec2 shift{x, 0};
        // The sheet's edge (ГОСТ 2.301, a thin line): on paper it is the paper itself.
        dxf::Entity edge;
        edge.kind = dxf::Kind::Polyline;
        edge.layer = kThin;
        edge.points = {shift, shift + Vec2{l.width, 0}, shift + Vec2{l.width, l.height}, shift + Vec2{0, l.height}};
        edge.bulges.assign(4, 0.0);
        edge.closed = true;
        out.entities.push_back(std::move(edge));
        for (const Stroke& s : l.strokes) {
            dxf::Entity e = dxf::Entity::Line(s.a + shift, s.b + shift, s.weight == LineWeight::Thick ? kThick : kThin);
            out.entities.push_back(std::move(e));
        }
        for (const Polyline& p : l.polylines) AddDashed(out, p.points, DashOf(p.style), LayerOf(p.weight, p.style), shift);
        for (const Fill& f : l.fills) {   // arrows: their outline, closed
            dxf::Entity e;
            e.kind = dxf::Kind::Polyline;
            e.layer = kThin;
            for (Vec2 p : f.points) e.points.push_back(p + shift);
            e.bulges.assign(e.points.size(), 0.0);
            e.closed = true;
            out.entities.push_back(std::move(e));
        }
        for (const Label& label : l.labels) {
            if (label.text.empty()) continue;
            Vec2 at = label.at;
            if (label.align == Align::Center) {   // DXF TEXT starts at its left end
                const double a = label.rotation * kPi / 180.0;
                double w = TextWidth(label.text, label.height);
                if (label.maxWidth > 0) w = std::min(w, label.maxWidth);
                at = at - Vec2{std::cos(a), std::sin(a)} * (w / 2);
            }
            out.entities.push_back(dxf::Entity::Text(at + shift, label.height, label.rotation, label.text, kText));
        }
        x += l.width + kDxfSheetGap;
    }
    return dxf::WriteR12(out);
}

Status ExportDrawingDxf(const Drawing& drawing, const std::filesystem::path& target, const SheetContent& content) {
    const std::string text = DrawingDxf(drawing, content);
    std::filesystem::path temp = target;
    temp += ".part";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!out) {
            std::error_code ec;
            std::filesystem::remove(temp, ec);
            return Error{"Не удалось записать DXF в выбранную папку"};
        }
    }
    std::error_code ec;
    std::filesystem::rename(temp, target, ec);
    if (ec) {
        std::filesystem::remove(temp, ec);
        return Error{"Не удалось записать DXF в выбранную папку"};
    }
    return Status::Ok();
}

} // namespace bz::drawing
