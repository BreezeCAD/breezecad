#include "drawing/SheetLayout.h"

#include <cmath>
#include <format>
#include <utility>

namespace bz::drawing {

namespace {

// ГОСТ 2.301 (ISO 216): the short and the long side, mm.
std::pair<double, double> Sides(SheetSize size) {
    switch (size) {
        case SheetSize::A4: return {210, 297};
        case SheetSize::A3: return {297, 420};
        case SheetSize::A2: return {420, 594};
        case SheetSize::A1: return {594, 841};
        case SheetSize::A0: return {841, 1189};
    }
    return {297, 420};
}

constexpr double kBindingMargin = 20;   // ГОСТ 2.104: the frame 20 mm from the left edge (binding)
constexpr double kMargin        = 5;    // and 5 mm from the others
constexpr double kBlockWidth    = 185;  // form 1
constexpr double kBlockHeight   = 55;
constexpr double kStripLeft     = 8;    // the additional graphs 19–23: a 12 mm strip left of the frame (5 + 7)

/// Builds strokes and labels in the title block's own coordinates: origin at
/// its lower right corner, x to the left (as KiCad's sheet measures them).
struct Block {
    SheetLayout& out;
    Vec2 corner;   ///< the lower right corner of the title block on the sheet

    Vec2 P(double x, double y) const { return {corner.x - x, corner.y + y}; }
    void Line(double x0, double y0, double x1, double y1, LineWeight w) { out.strokes.push_back({P(x0, y0), P(x1, y1), w}); }
    void Caption(std::string text, double x, double y, Align align = Align::Center) {
        // KiCad gives the middle of the text's height; the base line is a little lower.
        out.labels.push_back({std::move(text), {}, P(x, y - 1.25), 2.5, align});
    }
    void Value(const Sheet& s, const char* key, double x, double y, double height, Align align, double maxWidth) {
        const auto it = s.title.fields.find(key);
        out.labels.push_back({it != s.title.fields.end() ? it->second : std::string{}, key, P(x, y - height / 2), height, align, 0, maxWidth});
    }
};

} // namespace

Vec2 SheetSizeMm(SheetSize size, Orientation orientation) {
    const auto [shortSide, longSide] = Sides(size);
    if (size == SheetSize::A4 || orientation == Orientation::Portrait) return {shortSide, longSide};
    return {longSide, shortSide};
}

SheetLayout LayoutSheet(const Sheet& sheet, int number, int count) {
    SheetLayout out;
    const Vec2 size = SheetSizeMm(sheet.size, sheet.orientation);
    out.width  = size.x;
    out.height = size.y;
    const double left = kBindingMargin, right = size.x - kMargin, bottom = kMargin, top = size.y - kMargin;
    out.frame      = {{left, bottom}, {right, top}};
    out.titleBlock = {{right - kBlockWidth, bottom}, {right, bottom + kBlockHeight}};

    // The frame: a solid main line.
    out.strokes.push_back({{left, bottom}, {right, bottom}});
    out.strokes.push_back({{right, bottom}, {right, top}});
    out.strokes.push_back({{right, top}, {left, top}});
    out.strokes.push_back({{left, top}, {left, bottom}});

    // Form 1 (185 × 55). x from the right edge of the frame, y from its bottom.
    Block b{out, {right, bottom}};
    constexpr auto T = LineWeight::Thick, t = LineWeight::Thin;
    b.Line(185, 0, 185, 55, T);   // left edge
    b.Line(185, 55, 0, 55, T);    // top edge
    b.Line(120, 55, 120, 0, T);   // the columns of the changes and signatures part
    b.Line(130, 55, 130, 0, T);
    b.Line(145, 55, 145, 0, T);
    b.Line(168, 55, 168, 0, T);
    b.Line(178, 55, 178, 30, T);
    for (double y : {40.0, 45.0, 50.0}) b.Line(185, y, 120, y, t);   // rows of changes
    for (double y : {30.0, 35.0}) b.Line(185, y, 120, y, T);
    for (double y : {5.0, 10.0, 15.0, 20.0, 25.0}) b.Line(185, y, 120, y, t);   // rows of signatures
    b.Line(120, 40, 0, 40, T);    // graph 2 (designation) above, graphs 1/3 and 4–9 below
    b.Line(120, 15, 0, 15, T);    // graph 3 (material) below graph 1
    b.Line(50, 40, 50, 0, T);     // graphs 4–9 on the right
    b.Line(50, 35, 0, 35, T);     // «Лит. | Масса | Масштаб» captions
    b.Line(50, 20, 0, 20, T);     // «Лист | Листов»
    b.Line(35, 40, 35, 20, T);    // Масса | Масштаб
    b.Line(18, 40, 18, 20, T);
    for (double x : {45.0, 40.0}) b.Line(x, 35, x, 20, t);   // three cells of the letter
    b.Line(30, 20, 30, 15, T);    // Лист | Листов

    b.Caption("Изм.", 181.5, 32.5);
    b.Caption("Лист", 173, 32.5);
    b.Caption("№ докум.", 156.5, 32.5);
    b.Caption("Подп.", 137, 32.5);
    b.Caption("Дата", 125, 32.5);
    b.Caption("Разраб.", 184.5, 27.5, Align::Left);
    b.Caption("Пров.", 184.5, 22.5, Align::Left);
    b.Caption("Т.контр.", 184.5, 17.5, Align::Left);
    b.Caption("Н.контр.", 184.5, 7.5, Align::Left);
    b.Caption("Утв.", 184.5, 2.5, Align::Left);
    b.Caption("Лит.", 42, 37.5);
    b.Caption("Масса", 26.5, 37.5);
    b.Caption("Масштаб", 9, 37.5);
    b.Caption("Лист", 49, 17.5, Align::Left);
    b.Caption("Листов", 29, 17.5, Align::Left);

    // Field values. Graphs 1 and 2 in 7 mm letters (as a drawing is filled in), the rest smaller.
    b.Value(sheet, field::kDesignation, 60, 47.5, 7, Align::Center, 118);
    b.Value(sheet, field::kName, 85, 27.5, 7, Align::Center, 68);
    b.Value(sheet, field::kMaterial, 85, 7.5, 5, Align::Center, 68);
    b.Value(sheet, field::kLetter, 45, 27.5, 3.5, Align::Center, 14);
    b.Value(sheet, field::kMass, 26.5, 27.5, 3.5, Align::Center, 16);
    b.Value(sheet, field::kScale, 9, 27.5, 3.5, Align::Center, 17);
    b.Value(sheet, field::kOrganization, 25, 7.5, 3.5, Align::Center, 48);
    b.Value(sheet, field::kDeveloper, 167.5, 27.5, 2.5, Align::Left, 22);
    b.Value(sheet, field::kChecker, 167.5, 22.5, 2.5, Align::Left, 22);
    b.Value(sheet, field::kTechControl, 167.5, 17.5, 2.5, Align::Left, 22);
    b.Value(sheet, field::kNormControl, 167.5, 7.5, 2.5, Align::Left, 22);
    b.Value(sheet, field::kApprover, 167.5, 2.5, 2.5, Align::Left, 22);
    out.labels.push_back({std::to_string(number), {}, b.P(35, 16.25), 2.5, Align::Center});
    out.labels.push_back({std::to_string(count), {}, b.P(8, 16.25), 2.5, Align::Center});

    // Additional graphs 19–23 left of the frame, bottom up (Инв. № подл. … Подп. и дата).
    const double x0 = kStripLeft, x1 = kStripLeft + 5;
    const double rows[] = {0, 25, 60, 85, 110, 145};
    out.strokes.push_back({{x0, bottom}, {x0, bottom + 145}});
    out.strokes.push_back({{x1, bottom}, {x1, bottom + 145}});
    for (double y : rows) out.strokes.push_back({{x0, bottom + y}, {left, bottom + y}});
    const std::pair<const char*, double> strip[] = {
        {"Инв. № подл.", 12.5}, {"Подп. и дата", 42.5}, {"Взам. инв. №", 72.5}, {"Инв. № дубл.", 97.5}, {"Подп. и дата", 127.5}};
    for (const auto& [text, y] : strip) out.labels.push_back({text, {}, {x0 + 3.5, bottom + y}, 2.5, Align::Center, 90, 0});

    // Graphs 24–25 at the top left: «Перв. примен.», «Справ. №».
    out.strokes.push_back({{x0, top}, {x0, top - 120}});
    out.strokes.push_back({{x1, top}, {x1, top - 120}});
    for (double y : {0.0, 60.0, 120.0}) out.strokes.push_back({{x0, top - y}, {left, top - y}});
    out.labels.push_back({"Перв. примен.", {}, {x0 + 3.5, top - 30}, 2.5, Align::Center, 90, 0});
    out.labels.push_back({"Справ. №", {}, {x0 + 3.5, top - 90}, 2.5, Align::Center, 90, 0});

    // Graph 26 at the top left inside the frame: the designation turned by 180°.
    out.strokes.push_back({{left, top - 14}, {left + 70, top - 14}});
    out.strokes.push_back({{left + 70, top - 14}, {left + 70, top}});
    {
        const auto it = sheet.title.fields.find(field::kDesignation);
        out.labels.push_back({it != sheet.title.fields.end() ? it->second : std::string{}, field::kDesignation, {left + 35, top - 7 + 1.75}, 3.5,
                              Align::Center, 180, 68});
    }

    // Under the frame: «Копировал» and the size.
    out.labels.push_back({"Копировал", {}, {right - 110, 1.25}, 2.5, Align::Left});
    out.labels.push_back({std::format("Формат {}", SizeId(sheet.size)), {}, {right - 40, 1.25}, 2.5, Align::Left});
    return out;
}

namespace {

constexpr double kAxisOverrun = 3;     // ГОСТ 2.305: centre lines go 2–5 mm past the contour
constexpr double kSmallCircle = 12;    // ГОСТ 2.303: below this diameter, centre lines are solid thin

} // namespace

void AddView(SheetLayout& out, const View& view, const ViewLines& lines) {
    const double s = view.scale.Value();
    const auto place = [&](Vec2 p) { return view.at + p * s; };
    const auto add = [&](const std::vector<Vec2>& points, LineWeight w, LineStyle style) {
        if (points.size() < 2) return;
        Polyline line{{}, w, style};
        line.points.reserve(points.size());
        for (Vec2 p : points) line.points.push_back(place(p));
        out.polylines.push_back(std::move(line));
    };
    if (view.hidden)
        for (const auto& l : lines.hidden) add(l, LineWeight::Thin, LineStyle::Dashed);
    for (const auto& l : lines.visible) add(l, LineWeight::Thick, LineStyle::Solid);
    if (!view.axes) return;
    for (const ViewLines::Mark& m : lines.marks) {
        const Vec2 c = place(m.center);
        const double r = m.radius * s + kAxisOverrun;
        const LineStyle style = 2 * m.radius * s < kSmallCircle ? LineStyle::Solid : LineStyle::DashDot;
        out.polylines.push_back({{c - Vec2{r, 0}, c + Vec2{r, 0}}, LineWeight::Thin, style});
        out.polylines.push_back({{c - Vec2{0, r}, c + Vec2{0, r}}, LineWeight::Thin, style});
    }
    for (const ViewLines::Axis& a : lines.axes) {
        const Vec2 p = place(a.a), q = place(a.b), d = q - p;
        const double len = std::hypot(d.x, d.y);
        if (len < 1e-9) continue;
        const Vec2 step = d * (kAxisOverrun / len);
        out.polylines.push_back({{p - step, q + step}, LineWeight::Thin, LineStyle::DashDot});
    }
}

Box2 ViewBox(const View& view, const ViewLines& lines) {
    SheetLayout l;
    AddView(l, view, lines);
    Box2 box;
    for (const Polyline& p : l.polylines)
        for (Vec2 v : p.points) box.Include(v);
    return box;
}

SheetLayout LayoutSheetWithViews(const Sheet& sheet, int number, int count, const ViewLinesSource& lines) {
    SheetLayout out = LayoutSheet(sheet, number, count);
    if (!lines) return out;
    for (const View& v : ViewsOf(sheet))
        if (const ViewLines* l = lines(sheet, v)) AddView(out, v, *l);
    return out;
}

} // namespace bz::drawing
