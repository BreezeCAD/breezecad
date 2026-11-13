#include "drawing/SheetLayout.h"

#include <algorithm>
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

/// A field as the sheet shows it: the user's value, else the model's.
std::string FieldValue(const Sheet& s, const char* key, const Fields* automatic) {
    if (const auto it = s.title.fields.find(key); it != s.title.fields.end()) return it->second;
    if (automatic)
        if (const auto it = automatic->find(key); it != automatic->end()) return it->second;
    return {};
}

/// Builds strokes and labels in the title block's own coordinates: origin at
/// its lower right corner, x to the left (as KiCad's sheet measures them).
struct Block {
    SheetLayout& out;
    Vec2 corner;   ///< the lower right corner of the title block on the sheet
    const Fields* automatic = nullptr;   ///< values of the fields the sheet does not set

    Vec2 P(double x, double y) const { return {corner.x - x, corner.y + y}; }
    void Line(double x0, double y0, double x1, double y1, LineWeight w) { out.strokes.push_back({P(x0, y0), P(x1, y1), w}); }
    void Caption(std::string text, double x, double y, Align align = Align::Center) {
        // KiCad gives the middle of the text's height; the base line is a little lower.
        out.labels.push_back({std::move(text), {}, P(x, y - 1.25), 2.5, align});
    }
    void Value(const Sheet& s, const char* key, double x, double y, double height, Align align, double maxWidth) {
        out.labels.push_back({FieldValue(s, key, automatic), key, P(x, y - height / 2), height, align, 0, maxWidth});
    }
    void Text(std::string text, double x, double y, double height = 2.5, Align align = Align::Center, double maxWidth = 0) {
        out.labels.push_back({std::move(text), {}, P(x, y - height / 2), height, align, 0, maxWidth});
    }
};

} // namespace

Vec2 SheetSizeMm(SheetSize size, Orientation orientation) {
    const auto [shortSide, longSide] = Sides(size);
    if (size == SheetSize::A4 || orientation == Orientation::Portrait) return {shortSide, longSide};
    return {longSide, shortSide};
}

namespace {

/// The sheet of `size`: the frame (a solid main line), the additional graphs
/// 19–23 left of it; on a first sheet also 24–25, and graph 26 with the
/// designation on a drawing's; under it «Копировал» and the format.
/// `blockHeight`: of the title block.
SheetLayout Frame(const Sheet& sheet, Vec2 size, double blockHeight, bool first, bool graph26, const Fields* automatic) {
    SheetLayout out;
    out.width  = size.x;
    out.height = size.y;
    const double left = kBindingMargin, right = size.x - kMargin, bottom = kMargin, top = size.y - kMargin;
    out.frame      = {{left, bottom}, {right, top}};
    out.titleBlock = {{right - kBlockWidth, bottom}, {right, bottom + blockHeight}};
    out.strokes.push_back({{left, bottom}, {right, bottom}});
    out.strokes.push_back({{right, bottom}, {right, top}});
    out.strokes.push_back({{right, top}, {left, top}});
    out.strokes.push_back({{left, top}, {left, bottom}});

    // Additional graphs 19–23 left of the frame, bottom up (Инв. № подл. … Подп. и дата).
    const double x0 = kStripLeft, x1 = kStripLeft + 5;
    const double rows[] = {0, 25, 60, 85, 110, 145};
    out.strokes.push_back({{x0, bottom}, {x0, bottom + 145}});
    out.strokes.push_back({{x1, bottom}, {x1, bottom + 145}});
    for (double y : rows) out.strokes.push_back({{x0, bottom + y}, {left, bottom + y}});
    const std::pair<const char*, double> strip[] = {
        {"Инв. № подл.", 12.5}, {"Подп. и дата", 42.5}, {"Взам. инв. №", 72.5}, {"Инв. № дубл.", 97.5}, {"Подп. и дата", 127.5}};
    for (const auto& [text, y] : strip) out.labels.push_back({text, {}, {x0 + 3.5, bottom + y}, 2.5, Align::Center, 90, 0});

    if (first) {
        // Graphs 24–25 at the top left: «Перв. примен.», «Справ. №».
        out.strokes.push_back({{x0, top}, {x0, top - 120}});
        out.strokes.push_back({{x1, top}, {x1, top - 120}});
        for (double y : {0.0, 60.0, 120.0}) out.strokes.push_back({{x0, top - y}, {left, top - y}});
        out.labels.push_back({"Перв. примен.", {}, {x0 + 3.5, top - 30}, 2.5, Align::Center, 90, 0});
        out.labels.push_back({"Справ. №", {}, {x0 + 3.5, top - 90}, 2.5, Align::Center, 90, 0});
    }
    if (first && graph26) {
        // Graph 26 at the top left inside the frame: the designation turned by 180°.
        out.strokes.push_back({{left, top - 14}, {left + 70, top - 14}});
        out.strokes.push_back({{left + 70, top - 14}, {left + 70, top}});
        out.labels.push_back({FieldValue(sheet, field::kDesignation, automatic), field::kDesignation, {left + 35, top - 7 + 1.75}, 3.5,
                              Align::Center, 180, 68});
    }

    // Under the frame: «Копировал» and the size.
    out.labels.push_back({"Копировал", {}, {right - 110, 1.25}, 2.5, Align::Left});
    out.labels.push_back({std::format("Формат {}", SizeId(sheet.size)), {}, {right - 40, 1.25}, 2.5, Align::Left});
    return out;
}

} // namespace

SheetLayout LayoutSheet(const Sheet& sheet, int number, int count, const Fields* automatic) {
    const Vec2 size = SheetSizeMm(sheet.size, sheet.orientation);
    SheetLayout out = Frame(sheet, size, kBlockHeight, true, true, automatic);
    const double right = size.x - kMargin, bottom = kMargin;

    // Form 1 (185 × 55). x from the right edge of the frame, y from its bottom.
    Block b{out, {right, bottom}, automatic};
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
    // A section: its cut faces hatched (under the contour lines).
    for (const auto& face : lines.sections) {
        std::vector<std::vector<Vec2>> loops;
        Box2 box;
        for (const auto& loop : face) {
            std::vector<Vec2> on;
            for (Vec2 p : loop) {
                on.push_back(place(p));
                box.Include(on.back());
            }
            loops.push_back(std::move(on));
        }
        if (box.IsEmpty()) continue;
        // ГОСТ 2.306: 1–10 mm apart, by the size of the face (chosen: a tenth of its smaller side, 1.5–5 mm).
        const double step = std::clamp(std::min(box.max.x - box.min.x, box.max.y - box.min.y) / 10, 1.5, 5.0);
        for (Polyline& p : Hatch(loops, step)) out.polylines.push_back(std::move(p));
    }
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

std::vector<Polyline> Hatch(const std::vector<std::vector<Vec2>>& loops, double step, double angleDeg) {
    std::vector<Polyline> out;
    if (step <= 0) return out;
    const double a = angleDeg * kPi / 180.0;
    const Vec2 dir{std::cos(a), std::sin(a)}, across{-dir.y, dir.x};
    // Lines along `dir`, one every `step` across it; each cut where it crosses the loops.
    double lo = 1e300, hi = -1e300;
    for (const auto& l : loops)
        for (Vec2 p : l) {
            lo = std::min(lo, p.Dot(across));
            hi = std::max(hi, p.Dot(across));
        }
    if (lo > hi) return out;
    for (double c = std::ceil(lo / step) * step; c <= hi; c += step) {
        std::vector<double> ts;
        for (const auto& l : loops)
            for (std::size_t i = 0; i < l.size(); ++i) {
                const Vec2 p = l[i], q = l[(i + 1) % l.size()];
                const double dp = p.Dot(across) - c, dq = q.Dot(across) - c;
                if ((dp < 0) == (dq < 0)) continue;   // the half-open rule: a vertex on the line counts once
                const double t = dp / (dp - dq);
                ts.push_back((p + (q - p) * t).Dot(dir));
            }
        std::sort(ts.begin(), ts.end());
        for (std::size_t i = 0; i + 1 < ts.size(); i += 2) {
            if (ts[i + 1] - ts[i] < 1e-9) continue;
            out.push_back({{across * c + dir * ts[i], across * c + dir * ts[i + 1]}, LineWeight::Thin, LineStyle::Solid});
        }
    }
    return out;
}

void AddCuttingLine(SheetLayout& out, const View& parent, const View& section) {
    if (!section.section) return;
    const SectionCut& cut = *section.section;
    const double s = parent.scale.Value();
    const Vec2 a = parent.at + cut.a * s, b = parent.at + cut.b * s;
    const Vec2 ab = b - a;
    const double len = ab.Length();
    if (len < 1e-6) return;
    const Vec2 t = ab / len;
    // The sight on the parent's sheet: the cut's sight (−eye) in the parent's plane.
    const ViewBasis pb = BasisOf(parent);
    const Vec3 right = pb.up.Cross(pb.eye).Normalized();
    const Vec3 sight = cut.eye.Normalized() * -1;
    Vec2 n{sight.Dot(right), sight.Dot(pb.up)};
    if (n.Length() < 1e-6) return;
    n = n.Normalized();
    // ГОСТ 2.303: strokes 8–20 mm (chosen 10, shorter on a short line); the arrow
    // 2–3 mm from the stroke's outer end, its tip on the stroke (chosen 2.5).
    const double stroke = std::min(10.0, len / 3);
    constexpr double kArrowShaft = 8, kLetterHeight = 7;   // letters about twice the dimension figures (ГОСТ 2.316, not checked)
    for (const auto& [end, inward] : {std::pair{a, t}, std::pair{b, t * -1}}) {
        out.strokes.push_back({end, end + inward * stroke, LineWeight::Thick});
        const Vec2 tip = end + inward * 2.5;
        const Vec2 tail = tip - n * kArrowShaft;
        out.polylines.push_back({{tail, tip}, LineWeight::Thin, LineStyle::Solid});
        const Vec2 back = tip - n * kArrowLength, w = Vec2{-n.y, n.x} * kArrowHalfWidth;
        out.fills.push_back({{tip, back + w, back - w}});
        Label l;
        l.text   = cut.letter;
        l.height = kLetterHeight;
        l.align  = Align::Center;
        l.at     = tail - inward * 4 - Vec2{0, kLetterHeight / 2};
        out.labels.push_back(std::move(l));
    }
}

void AddSectionLabel(SheetLayout& out, const View& section, const Box2& box) {
    if (!section.section || box.IsEmpty()) return;
    Label l;
    l.text   = section.section->letter + "\u2013" + section.section->letter;   // «А–А», ГОСТ 2.305
    l.height = 7;
    l.align  = Align::Center;
    l.at     = {(box.min.x + box.max.x) / 2, box.max.y + 5};
    out.labels.push_back(std::move(l));
}

Box2 ViewBox(const View& view, const ViewLines& lines) {
    SheetLayout l;
    AddView(l, view, lines);
    Box2 box;
    for (const Polyline& p : l.polylines)
        for (Vec2 v : p.points) box.Include(v);
    return box;
}

SheetLayout LayoutSpecPage(const Sheet& sheet, const std::vector<SpecRow>& rows, int page, int pages, const Fields* automatic) {
    const Vec2 size = SheetSizeMm(SheetSize::A4, Orientation::Portrait);
    const bool first = page == 0;
    // The table takes the whole width under the frame's top: no graph 26 (its designation is in the title block).
    SheetLayout out = Frame(sheet, size, first ? 40 : 15, first, false, automatic);
    const double left = kBindingMargin, right = size.x - kMargin, bottom = kMargin, top = size.y - kMargin;
    constexpr auto T = LineWeight::Thick, t = LineWeight::Thin;
    Block b{out, {right, bottom}, automatic};

    if (first) {
        // Form 2 (185 × 40), ГОСТ 2.104: the changes and signatures on the left
        // as in form 1, then the designation, the name, «Лит. | Лист | Листов»
        // and the organization.
        b.Line(185, 0, 185, 40, T);
        b.Line(185, 40, 0, 40, T);
        for (double x : {120.0, 130.0, 145.0, 168.0}) b.Line(x, 40, x, 0, T);
        b.Line(178, 40, 178, 25, T);
        b.Line(185, 35, 120, 35, t);
        for (double y : {25.0, 30.0}) b.Line(185, y, 120, y, T);
        for (double y : {5.0, 10.0, 15.0, 20.0}) b.Line(185, y, 120, y, t);
        b.Line(120, 25, 0, 25, T);   // graph 2 above
        b.Line(50, 25, 50, 0, T);    // graph 1 left of it, graphs 4, 7, 8 right
        b.Line(50, 20, 0, 20, T);
        b.Line(50, 15, 0, 15, T);
        b.Line(35, 25, 35, 15, T);
        b.Line(20, 25, 20, 15, T);
        for (double x : {45.0, 40.0}) b.Line(x, 20, x, 15, t);
        b.Caption("Изм.", 181.5, 27.5);
        b.Caption("Лист", 173, 27.5);
        b.Caption("№ докум.", 156.5, 27.5);
        b.Caption("Подп.", 137, 27.5);
        b.Caption("Дата", 125, 27.5);
        b.Caption("Разраб.", 184.5, 22.5, Align::Left);
        b.Caption("Пров.", 184.5, 17.5, Align::Left);
        b.Caption("Н.контр.", 184.5, 7.5, Align::Left);
        b.Caption("Утв.", 184.5, 2.5, Align::Left);
        b.Caption("Лит.", 42.5, 22.5);
        b.Caption("Лист", 27.5, 22.5);
        b.Caption("Листов", 10, 22.5);
        b.Value(sheet, field::kDesignation, 60, 32.5, 7, Align::Center, 118);
        b.Value(sheet, field::kName, 85, 12.5, 5, Align::Center, 68);
        b.Value(sheet, field::kLetter, 42.5, 17.5, 2.5, Align::Center, 14);
        b.Value(sheet, field::kOrganization, 25, 7.5, 3.5, Align::Center, 48);
        b.Value(sheet, field::kDeveloper, 167.5, 22.5, 2.5, Align::Left, 22);
        b.Value(sheet, field::kChecker, 167.5, 17.5, 2.5, Align::Left, 22);
        b.Value(sheet, field::kNormControl, 167.5, 7.5, 2.5, Align::Left, 22);
        b.Value(sheet, field::kApprover, 167.5, 2.5, 2.5, Align::Left, 22);
        if (pages > 1) b.Text(std::to_string(page + 1), 27.5, 17.5);   // «Лист» only when there are several
        b.Text(std::to_string(pages), 10, 17.5);
    } else {
        // Form 2a (185 × 15): changes, the designation, «Лист».
        b.Line(185, 0, 185, 15, T);
        b.Line(185, 15, 0, 15, T);
        for (double x : {120.0, 130.0, 145.0, 168.0, 178.0, 10.0}) b.Line(x, 15, x, 0, T);
        b.Line(185, 10, 120, 10, t);
        b.Line(185, 5, 120, 5, T);
        b.Line(10, 8, 0, 8, T);
        b.Caption("Изм.", 181.5, 2.5);
        b.Caption("Лист", 173, 2.5);
        b.Caption("№ докум.", 156.5, 2.5);
        b.Caption("Подп.", 137, 2.5);
        b.Caption("Дата", 125, 2.5);
        b.Caption("Лист", 5, 11.5);
        b.Value(sheet, field::kDesignation, 65, 7.5, 5, Align::Center, 108);
        b.Text(std::to_string(page + 1), 5, 4);
    }

    // The table (ГОСТ 2.106 form 1): columns 6, 6, 8, 70, 63, 10, 22 mm;
    // a 15 mm head, rows of 8 mm down to the title block.
    const double tableBottom = bottom + (first ? 40 : 15);
    const double head = top - 15;
    const int capacity = first ? kSpecRowsFirst : kSpecRowsNext;
    const double xs[] = {left, left + 6, left + 12, left + 20, left + 90, left + 153, left + 163, right};
    for (std::size_t i = 1; i + 1 < std::size(xs); ++i) out.strokes.push_back({{xs[i], top}, {xs[i], tableBottom}});
    out.strokes.push_back({{left, head}, {right, head}});
    for (int r = 1; r <= capacity; ++r) {
        const double y = head - 8.0 * r;
        if (y <= tableBottom + 1e-6) break;
        out.strokes.push_back({{left, y}, {right, y}, LineWeight::Thin});
    }
    // The head: the narrow columns' names turned upright.
    const auto upright = [&](const char* text, int column) {
        out.labels.push_back({text, {}, {(xs[column] + xs[column + 1]) / 2 + 1.25, head + 7.5}, 2.5, Align::Center, 90, 14});
    };
    upright("Формат", 0);
    upright("Зона", 1);
    upright("Поз.", 2);
    upright("Кол.", 5);
    const auto across = [&](const char* text, int column) {
        out.labels.push_back({text, {}, {(xs[column] + xs[column + 1]) / 2, head + 6.25}, 3.5, Align::Center, 0, xs[column + 1] - xs[column] - 2});
    };
    across("Обозначение", 3);
    across("Наименование", 4);
    across("Примечание", 6);

    // The rows of this page.
    const std::size_t from = first ? 0 : static_cast<std::size_t>(kSpecRowsFirst + (page - 1) * kSpecRowsNext);
    for (int r = 0; r < capacity && from + static_cast<std::size_t>(r) < rows.size(); ++r) {
        const SpecRow& row = rows[from + static_cast<std::size_t>(r)];
        const double base = head - 8.0 * (r + 1) + 2.25;   // the base line of 3.5 mm letters in an 8 mm row
        const auto cell = [&](const std::string& text, int column, Align align) {
            if (text.empty()) return;
            const double w = xs[column + 1] - xs[column] - 2;
            const double x = align == Align::Center ? (xs[column] + xs[column + 1]) / 2 : xs[column] + 1;
            out.labels.push_back({text, {}, {x, base}, 3.5, align, 0, w});
        };
        if (row.kind == SpecRow::Kind::Section) {
            // A section's title in «Наименование», in the middle, underlined (ГОСТ 2.106).
            cell(row.name, 4, Align::Center);
            const double half = std::min(30.0, 1.0 + 1.8 * static_cast<double>(row.name.size()) / 2);   // about the title's width
            const double mid = (xs[4] + xs[5]) / 2;
            out.strokes.push_back({{mid - half, base - 0.8}, {mid + half, base - 0.8}, LineWeight::Thin});
            continue;
        }
        cell(row.format, 0, Align::Center);
        cell(row.zone, 1, Align::Center);
        cell(row.position, 2, Align::Center);
        cell(row.designation, 3, Align::Left);
        cell(row.name, 4, Align::Left);
        cell(row.quantity, 5, Align::Center);
        cell(row.note, 6, Align::Left);
    }
    return out;
}

std::vector<Page> PagesOf(const Drawing& drawing, std::size_t sheet, const SheetContent& content) {
    std::vector<Page> out;
    for (const Page& p : PagesOf(drawing, content))
        if (p.sheet == sheet) out.push_back(p);
    return out;
}

std::vector<Page> PagesOf(const Drawing& drawing, const SheetContent& content) {
    std::vector<Page> out;
    int drawings = 0;
    for (const Sheet& s : drawing.sheets) drawings += s.kind == SheetKind::Drawing ? 1 : 0;
    int number = 0;
    for (std::size_t i = 0; i < drawing.sheets.size(); ++i) {
        const Sheet& s = drawing.sheets[i];
        if (s.kind == SheetKind::Specification) {
            const std::size_t rows = content.spec ? content.spec(s).size() : 0;
            const int pages = SpecPageCount(rows);
            for (int p = 0; p < pages; ++p) out.push_back({i, p, p + 1, pages});
        } else {
            out.push_back({i, 0, ++number, drawings});
        }
    }
    return out;
}

SheetLayout LayoutPage(const Drawing& drawing, const Page& page, const SheetContent& content) {
    const Sheet& sheet = drawing.sheets.at(page.sheet);
    Fields automatic;
    if (content.fields) automatic = content.fields(sheet);
    if (sheet.kind == SheetKind::Specification) {
        const std::vector<SpecRow> rows = content.spec ? content.spec(sheet) : std::vector<SpecRow>{};
        return LayoutSpecPage(sheet, rows, page.page, page.count, &automatic);
    }
    SheetLayout out = LayoutSheet(sheet, page.number, page.count, &automatic);
    const std::vector<View> views = ViewsOf(sheet);
    if (content.lines)
        for (const View& v : views)
            if (const ViewLines* l = content.lines(sheet, v)) {
                AddView(out, v, *l);
                AddSectionLabel(out, v, ViewBox(v, *l));
            }
    // Cutting lines of the sections, on the views they cut.
    for (const View& v : views)
        if (v.section)
            for (const View& parent : views)
                if (parent.id == v.section->of) AddCuttingLine(out, parent, v);
    // Dimensions over the views; one whose view is gone is not drawn (it stays in the file).
    if (content.edges)
        for (const Dimension& dim : DimensionsOf(sheet)) {
            const auto v = std::find_if(views.begin(), views.end(), [&](const View& x) { return x.id == dim.view; });
            if (v == views.end()) continue;
            const std::vector<std::optional<ModelEdge>> edges = content.edges(sheet, *v, dim.edges);
            const std::optional<Measure> m = MeasureOf(*v, dim, edges);
            std::vector<Vec2> lost;
            if (!m)
                for (const Json& e : dim.edges)   // where the edges were: their middle points
                    if (const auto mid = e.find("middle"); mid != e.end() && mid->is_array() && mid->size() == 3)
                        lost.push_back(SheetPointOf(*v, {(*mid)[0].get<double>(), (*mid)[1].get<double>(), (*mid)[2].get<double>()}));
            const Box2 box = AddDimension(out, *v, dim, m, lost);
            out.hits.push_back({dim.id, box});
        }
    return out;
}

} // namespace bz::drawing
