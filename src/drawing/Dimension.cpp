#include "drawing/Dimension.h"

#include "drawing/SheetLayout.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <utility>

namespace bz::drawing {

namespace {

constexpr std::array<std::pair<DimensionKind, const char*>, 4> kKinds{{{DimensionKind::Linear, "linear"},
                                                                         {DimensionKind::Diameter, "diameter"},
                                                                         {DimensionKind::Radius, "radius"},
                                                                         {DimensionKind::Angle, "angle"}}};
constexpr std::array<std::pair<DimensionAxis, const char*>, 3> kAxes{
    {{DimensionAxis::Horizontal, "horizontal"}, {DimensionAxis::Vertical, "vertical"}, {DimensionAxis::Aligned, "aligned"}}};

bool ReadId(const Json& j, const char* key, ObjectId& out) {
    const auto it = j.find(key);
    if (it == j.end()) return true;
    if (!it->is_number_unsigned()) return false;
    out = ObjectId{it->get<std::uint64_t>()};
    return true;
}

Vec2 Perp(Vec2 v) { return {-v.y, v.x}; }

/// A text turned along `dir` so it reads from the left or from below (ГОСТ 2.307).
double ReadableAngle(Vec2 dir) {
    double a = std::atan2(dir.y, dir.x) * 180.0 / kPi;
    if (a > 90.0 + 1e-6) a -= 180.0;
    if (a <= -90.0 + 1e-6) a += 180.0;
    return a;
}

/// The closest pair among two sets of points.
std::pair<Vec2, Vec2> Closest(const std::vector<Vec2>& a, const std::vector<Vec2>& b) {
    std::pair<Vec2, Vec2> best{a.front(), b.front()};
    double d = 1e300;
    for (Vec2 p : a)
        for (Vec2 q : b)
            if (const double l = (q - p).Length(); l < d) {
                d = l;
                best = {p, q};
            }
    return best;
}

/// Approximate width of a text on the sheet (the layout has no font): a
/// character about 0.8 of the capitals' height (IBM Plex figures at ГОСТ 2.304 size).
double TextWidth(const std::string& text, double height) {
    std::size_t chars = 0;
    for (const char c : text)
        if ((static_cast<unsigned char>(c) & 0xC0U) != 0x80U) ++chars;
    return static_cast<double>(chars) * height * 0.8;
}

struct Pen {
    SheetLayout& out;
    Box2&        box;
    bool         problem;

    void Line(Vec2 a, Vec2 b) {
        out.polylines.push_back({{a, b}, LineWeight::Thin, LineStyle::Solid, problem});
        box.Include(a);
        box.Include(b);
    }
    void Poly(std::vector<Vec2> points) {
        for (Vec2 p : points) box.Include(p);
        out.polylines.push_back({std::move(points), LineWeight::Thin, LineStyle::Solid, problem});
    }
    /// An arrow with its tip at `tip`, pointing along `dir` (unit).
    void Arrow(Vec2 tip, Vec2 dir) {
        const Vec2 back = tip - dir * kArrowLength, side = Perp(dir) * kArrowHalfWidth;
        Fill f{{tip, back + side, back - side}, problem};
        for (Vec2 p : f.points) box.Include(p);
        out.fills.push_back(std::move(f));
    }
    /// A text standing on a line along `dir` through `at`, centred there.
    void Text(const std::string& text, Vec2 at, Vec2 dir) {
        const double angle = ReadableAngle(dir);
        const double a = angle * kPi / 180.0;
        const Vec2 along{std::cos(a), std::sin(a)}, up = Perp(along);
        const Vec2 base = at + up * 1.0;   // over the line, a millimetre above it
        Label l;
        l.text     = text;
        l.at       = base;
        l.height   = kDimTextHeight;
        l.align    = Align::Center;
        l.rotation = angle;
        l.problem  = problem;
        out.labels.push_back(std::move(l));
        const double w = TextWidth(text, kDimTextHeight) / 2;
        for (Vec2 p : {base - along * w, base + along * w, base - along * w + up * kDimTextHeight, base + along * w + up * kDimTextHeight})
            box.Include(p);
    }
};

/// A linear dimension between `q1` and `q2` measured along `dir` (unit),
/// its line through `o`.
void LinearLines(Pen& pen, Vec2 q1, Vec2 q2, Vec2 dir, Vec2 o, const std::string& text) {
    const Vec2 n = Perp(dir);
    const Vec2 d1 = q1 + n * (o - q1).Dot(n), d2 = q2 + n * (o - q2).Dot(n);
    for (const auto& [q, d] : {std::pair{q1, d1}, std::pair{q2, d2}}) {
        const Vec2 e = d - q;
        const double len = e.Length();
        if (len > 1e-6) pen.Line(q, d + e * (kExtensionOver / len));
    }
    const Vec2 span = d2 - d1;
    const double len = span.Length();
    if (len < 1e-9) return;
    const Vec2 u = span / len;
    if (len >= 2 * kArrowLength + 1) {   // room inside: arrows inside, pointing out
        pen.Line(d1, d2);
        pen.Arrow(d1, u * -1);
        pen.Arrow(d2, u);
    } else {   // small: arrows outside, pointing in (ГОСТ 2.307)
        pen.Line(d1 - u * (kArrowLength + 3), d2 + u * (kArrowLength + 3));
        pen.Arrow(d1, u);
        pen.Arrow(d2, u * -1);
    }
    // The text over the middle of the line, or where the user put it along the line, outside the extension lines.
    const double t = (o - d1).Dot(u);
    const Vec2 at = t < 0 || t > len ? d1 + u * t : (d1 + d2) * 0.5;
    if (t < 0 || t > len) pen.Line(t < 0 ? d1 : d2, at + u * (t < 0 ? -1.0 : 1.0) * TextWidth(text, kDimTextHeight) / 2);
    pen.Text(text, at, u);
}

std::string Trim(double v, int decimals) {
    std::string s = std::format("{:.{}f}", v, decimals);
    if (s.find('.') != std::string::npos) {
        while (s.back() == '0') s.pop_back();
        if (s.back() == '.') s.pop_back();
    }
    for (char& c : s)
        if (c == '.') c = ',';
    if (s == "-0") s = "0";
    return s;
}

} // namespace

std::string_view KindId(DimensionKind kind) {
    for (const auto& [k, id] : kKinds)
        if (k == kind) return id;
    return "linear";
}
std::optional<DimensionKind> KindFromId(std::string_view id) {
    for (const auto& [k, name] : kKinds)
        if (name == id) return k;
    return std::nullopt;
}
std::string_view AxisId(DimensionAxis axis) {
    for (const auto& [a, id] : kAxes)
        if (a == axis) return id;
    return "aligned";
}
std::optional<DimensionAxis> AxisFromId(std::string_view id) {
    for (const auto& [a, name] : kAxes)
        if (name == id) return a;
    return std::nullopt;
}

std::optional<Dimension> ReadDimension(const Item& item) {
    const Json& j = item.json;
    if (!j.is_object() || item.Type() != kDimensionType) return std::nullopt;
    Dimension d;
    if (!ReadId(j, "id", d.id) || !d.id || !ReadId(j, "view", d.view)) return std::nullopt;
    const auto kind = j.find("kind");
    if (kind == j.end() || !kind->is_string()) return std::nullopt;
    const auto k = KindFromId(kind->get<std::string>());
    if (!k) return std::nullopt;   // a kind of a newer version: kept, not shown
    d.kind = *k;
    if (const auto a = j.find("axis"); a != j.end()) {
        if (!a->is_string()) return std::nullopt;
        const auto axis = AxisFromId(a->get<std::string>());
        if (!axis) return std::nullopt;
        d.axis = *axis;
    }
    const auto edges = j.find("edges");
    if (edges == j.end() || !edges->is_array() || edges->empty() || edges->size() > 2) return std::nullopt;
    for (const Json& e : *edges) {
        if (!e.is_object()) return std::nullopt;
        d.edges.push_back(e);
    }
    if (const auto at = j.find("at"); at != j.end()) {
        if (!at->is_array() || at->size() != 2 || !(*at)[0].is_number() || !(*at)[1].is_number()) return std::nullopt;
        d.at = {(*at)[0].get<double>(), (*at)[1].get<double>()};
    }
    if (const auto t = j.find("text"); t != j.end() && t->is_string()) d.text = t->get<std::string>();
    return d;
}

void WriteDimension(Item& item, const Dimension& d) {
    if (!item.json.is_object()) item.json = Json::object();
    Json& j = item.json;
    j["type"] = kDimensionType;
    j["id"]   = d.id.value;
    j["view"] = d.view.value;
    j["kind"] = std::string(KindId(d.kind));
    if (d.kind == DimensionKind::Linear) j["axis"] = std::string(AxisId(d.axis));
    else j.erase("axis");
    j["edges"] = Json(d.edges);
    j["at"]    = Json::array({d.at.x, d.at.y});
    if (d.text.empty()) j.erase("text");
    else j["text"] = d.text;
}

Item MakeDimensionItem(const Dimension& d) {
    Item item;
    WriteDimension(item, d);
    return item;
}

std::vector<Dimension> DimensionsOf(const Sheet& sheet) {
    std::vector<Dimension> out;
    for (const Item& item : sheet.annotations)
        if (auto d = ReadDimension(item)) out.push_back(*d);
    return out;
}

Item* FindAnnotation(Sheet& sheet, ObjectId id) {
    for (Item& item : sheet.annotations)
        if (item.json.is_object())
            if (const auto it = item.json.find("id"); it != item.json.end() && it->is_number_unsigned() && it->get<std::uint64_t>() == id.value)
                return &item;
    return nullptr;
}

Vec2 SheetPointOf(const View& view, Vec3 model) { return view.at + ProjectPoint(view, model) * view.scale.Value(); }
Vec2 ViewPointOf(const View& view, Vec2 sheet) { return (sheet - view.at) / view.scale.Value(); }
Vec2 DimensionAnchor(const View& view, const Dimension& dim) { return view.at + dim.at * view.scale.Value(); }

std::optional<Measure> MeasureOf(const View& view, const Dimension& dim, const std::vector<std::optional<ModelEdge>>& edges,
                                 std::string* why) {
    const auto fail = [&](const char* text) -> std::optional<Measure> {
        if (why) *why = text;
        return std::nullopt;
    };
    if (edges.size() != dim.edges.size() || edges.empty()) return fail("Нет рёбер размера");
    for (const auto& e : edges)
        if (!e) return fail("Ребро размера пропало из модели");
    const ViewBasis basis = BasisOf(view);
    const Vec3 eye = basis.eye.Normalized();
    const double s = view.scale.Value();
    const auto on = [&](Vec3 p) { return SheetPointOf(view, p); };
    // The ends of a circle seen from its side: across it, in the sheet's plane.
    const auto across = [&](const ModelEdge& c) -> std::pair<Vec2, Vec2> {
        Vec3 w = c.axis.Cross(eye);
        if (w.Length() < 1e-9) w = Vec3{1, 0, 0};
        w = w.Normalized() * c.radius;
        return {on(c.center + w), on(c.center - w)};
    };
    Measure m;
    m.kind = dim.kind;
    const ModelEdge& e1 = *edges[0];
    switch (dim.kind) {
        case DimensionKind::Diameter:
        case DimensionKind::Radius: {
            if (e1.kind != ModelEdge::Kind::Circle || e1.radius <= 0) return fail("Это не окружность и не дуга");
            m.prefix = dim.kind == DimensionKind::Diameter ? "Ø" : "R";
            m.value  = dim.kind == DimensionKind::Diameter ? 2 * e1.radius : e1.radius;
            m.center = on(e1.center);
            m.radius = e1.radius * s;
            m.endOn  = std::abs(e1.axis.Normalized().Dot(eye)) > 0.99;
            if (!m.endOn) std::tie(m.p1, m.p2) = across(e1);
            return m;
        }
        case DimensionKind::Angle: {
            if (edges.size() != 2 || e1.kind != ModelEdge::Kind::Line || edges[1]->kind != ModelEdge::Kind::Line)
                return fail("Угол ставится между двумя прямыми рёбрами");
            const Vec2 a1 = on(e1.a), b1 = on(e1.b), a2 = on(edges[1]->a), b2 = on(edges[1]->b);
            const Vec2 r1 = b1 - a1, r2 = b2 - a2;
            const double cross = r1.Cross(r2);
            if (r1.Length() < 1e-9 || r2.Length() < 1e-9 || std::abs(cross) < 1e-9 * r1.Length() * r2.Length())
                return fail("Рёбра угла параллельны");
            const double t = (a2 - a1).Cross(r2) / cross;
            m.center = a1 + r1 * t;
            // The sides of the sector the dimension line is in.
            const Vec2 o = DimensionAnchor(view, dim) - m.center;
            Vec2 u1 = r1.Normalized(), u2 = r2.Normalized();
            if (o.Length() > 1e-9) {
                const Vec2 od = o.Normalized();
                double best = -1;
                Vec2 bu1 = u1, bu2 = u2;
                for (double s1 : {1.0, -1.0})
                    for (double s2 : {1.0, -1.0}) {
                        const Vec2 v1 = u1 * s1, v2 = u2 * s2;
                        // O between v1 and v2 (the sector under 180°): on the same side of each as the other.
                        if (v1.Cross(od) * v1.Cross(v2) >= 0 && v2.Cross(od) * v2.Cross(v1) >= 0) {
                            const double score = od.Dot((v1 + v2).Normalized());
                            if (score > best) {
                                best = score;
                                bu1 = v1;
                                bu2 = v2;
                            }
                        }
                    }
                u1 = bu1;
                u2 = bu2;
            }
            m.u1 = u1;
            m.u2 = u2;
            m.value = std::acos(std::clamp(u1.Dot(u2), -1.0, 1.0)) * 180.0 / kPi;
            m.p1 = (a1 - m.center).Length() > (b1 - m.center).Length() ? a1 : b1;
            m.p2 = (a2 - m.center).Length() > (b2 - m.center).Length() ? a2 : b2;
            return m;
        }
        case DimensionKind::Linear: break;
    }
    // Linear: the points measured.
    const auto points = [&](const ModelEdge& e) -> std::vector<Vec2> {
        if (e.kind == ModelEdge::Kind::Circle) return {on(e.center)};
        return {on(e.a), on(e.b)};
    };
    if (edges.size() == 1) {
        if (e1.kind == ModelEdge::Kind::Circle) {
            if (std::abs(e1.axis.Normalized().Dot(eye)) > 0.99) return fail("Окружность видна целиком: поставьте диаметр");
            std::tie(m.p1, m.p2) = across(e1);
            m.prefix = "Ø";
        } else {
            m.p1 = on(e1.a);
            m.p2 = on(e1.b);
        }
    } else {
        const ModelEdge& e2 = *edges[1];
        const std::vector<Vec2> p = points(e1), q = points(e2);
        const bool lines = e1.kind != ModelEdge::Kind::Circle && e2.kind != ModelEdge::Kind::Circle;
        const Vec2 d1 = p.back() - p.front(), d2 = q.back() - q.front();
        if (lines && d1.Length() > 1e-9 && d2.Length() > 1e-9 &&
            std::abs(d1.Cross(d2)) < 1e-6 * d1.Length() * d2.Length() && dim.axis == DimensionAxis::Aligned) {
            // Two parallel lines: the distance between them, across.
            m.p1 = (p.front() + p.back()) * 0.5;
            const Vec2 u = d2.Normalized();
            m.p2 = q.front() + u * (m.p1 - q.front()).Dot(u);
        } else {
            std::tie(m.p1, m.p2) = Closest(p, q);
        }
    }
    const Vec2 d = m.p2 - m.p1;
    switch (dim.axis) {
        case DimensionAxis::Horizontal: m.value = std::abs(d.x) / s; break;
        case DimensionAxis::Vertical:   m.value = std::abs(d.y) / s; break;
        case DimensionAxis::Aligned:    m.value = d.Length() / s; break;
    }
    if (m.value < 1e-9) return fail("Размер нулевой: точки совпадают в этом направлении");
    return m;
}

std::string ValueText(const Measure& m) {
    if (m.kind == DimensionKind::Angle) {
        int minutes = static_cast<int>(std::lround(m.value * 60.0));
        const int deg = minutes / 60;
        minutes %= 60;
        return minutes == 0 ? std::format("{}°", deg) : std::format("{}°{}'", deg, minutes);
    }
    return Trim(m.value, 2);
}

std::string DimensionText(const Dimension& dim, const Measure& m) {
    const std::string value = m.prefix + ValueText(m);
    if (dim.text.empty()) return value;
    std::string out = dim.text;
    for (std::size_t at = out.find("<>"); at != std::string::npos; at = out.find("<>", at + value.size()))
        out.replace(at, 2, value);
    return out;
}

Box2 AddDimension(SheetLayout& out, const View& view, const Dimension& dim, const std::optional<Measure>& measure,
                  const std::vector<Vec2>& lost) {
    Box2 box;
    Pen pen{out, box, !measure};
    const Vec2 o = DimensionAnchor(view, dim);
    if (!measure) {
        // Lost: its text in red with a line to each place its edges were.
        const std::string text = dim.text.empty() || dim.text == "<>" ? "?" : dim.text;
        for (Vec2 p : lost) pen.Line(o, p);
        pen.Text(text, o, {1, 0});
        return box;
    }
    const Measure& m = *measure;
    const std::string text = DimensionText(dim, m);
    switch (m.kind) {
        case DimensionKind::Linear: {
            Vec2 dir{1, 0};
            if (dim.axis == DimensionAxis::Vertical) dir = {0, 1};
            if (dim.axis == DimensionAxis::Aligned) dir = (m.p2 - m.p1).Normalized();
            LinearLines(pen, m.p1, m.p2, dir, o, text);
            break;
        }
        case DimensionKind::Diameter:
        case DimensionKind::Radius: {
            if (!m.endOn) {   // a shaft or a hole from its side: across it, as a linear one
                LinearLines(pen, m.p1, m.p2, (m.p2 - m.p1).Normalized(), o, text);
                break;
            }
            Vec2 t = o - m.center;
            if (t.Length() < 1e-9) t = {1, 1};
            t = t.Normalized();
            const Vec2 rim = m.center + t * m.radius;
            const double reach = (o - m.center).Length();
            const double tw = TextWidth(text, kDimTextHeight);
            const bool outside = reach > m.radius;
            // The line from the centre (radius) or across (diameter), out to the text when it stands outside.
            const Vec2 from = m.kind == DimensionKind::Diameter ? m.center - t * m.radius : m.center;
            const Vec2 to = outside ? m.center + t * (reach + tw / 2) : rim;
            pen.Line(from, to);
            pen.Arrow(rim, t);
            if (m.kind == DimensionKind::Diameter) pen.Arrow(m.center - t * m.radius, t * -1);
            pen.Text(text, outside ? m.center + t * reach : m.center + t * (m.radius / 2), t);
            break;
        }
        case DimensionKind::Angle: {
            const double rho = std::max((o - m.center).Length(), kArrowLength * 2);
            const double a1 = std::atan2(m.u1.y, m.u1.x);
            double sweep = std::atan2(m.u1.Cross(m.u2), m.u1.Dot(m.u2));   // signed, under 180°
            const int steps = std::max(8, static_cast<int>(std::abs(sweep) * 180.0 / kPi / 3));
            std::vector<Vec2> arc;
            for (int i = 0; i <= steps; ++i) {
                const double a = a1 + sweep * i / steps;
                arc.push_back(m.center + Vec2{std::cos(a), std::sin(a)} * rho);
            }
            pen.Poly(arc);
            const Vec2 e1 = arc.front(), e2 = arc.back();
            // Extension lines from the edges out to the arc, when it passes beyond them.
            for (const auto& [p, e] : {std::pair{m.p1, e1}, std::pair{m.p2, e2}})
                if ((e - m.center).Length() > (p - m.center).Length() + 0.5) pen.Line(p, e + (e - m.center).Normalized() * kExtensionOver);
            const double sign = sweep >= 0 ? 1.0 : -1.0;
            pen.Arrow(e1, Perp(m.u1) * -sign);
            const Vec2 n2{std::cos(a1 + sweep), std::sin(a1 + sweep)};
            pen.Arrow(e2, Perp(n2) * sign);
            const double mid = a1 + sweep / 2;
            const Vec2 radial{std::cos(mid), std::sin(mid)};
            pen.Text(text, m.center + radial * rho, Perp(radial));
            break;
        }
    }
    return box;
}

} // namespace bz::drawing
