#include "drawing/Balloon.h"

#include "drawing/SheetLayout.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace bz::drawing {

namespace {

bool ReadId(const Json& j, const char* key, ObjectId& out) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_number_unsigned()) return false;
    out = ObjectId{it->get<std::uint64_t>()};
    return true;
}

/// About the width of a text on the sheet: a character 0.8 of the height (Dimension.cpp does the same).
double TextWidth(const std::string& text, double height) {
    std::size_t chars = 0;
    for (const char c : text)
        if ((static_cast<unsigned char>(c) & 0xC0U) != 0x80U) ++chars;
    return static_cast<double>(chars) * height * 0.8;
}

} // namespace

std::optional<Balloon> ReadBalloon(const Item& item) {
    const Json& j = item.json;
    if (!j.is_object() || item.Type() != kBalloonType) return std::nullopt;
    Balloon b;
    if (!ReadId(j, "id", b.id) || !ReadId(j, "view", b.view) || !ReadId(j, "component", b.component)) return std::nullopt;
    const auto p = j.find("point");
    if (p == j.end() || !p->is_array() || p->size() != 3 || !(*p)[0].is_number() || !(*p)[1].is_number() || !(*p)[2].is_number())
        return std::nullopt;
    b.point = {(*p)[0].get<double>(), (*p)[1].get<double>(), (*p)[2].get<double>()};
    const auto at = j.find("at");
    if (at == j.end() || !at->is_array() || at->size() != 2 || !(*at)[0].is_number() || !(*at)[1].is_number()) return std::nullopt;
    b.at = {(*at)[0].get<double>(), (*at)[1].get<double>()};
    return b;
}

void WriteBalloon(Item& item, const Balloon& b) {
    if (!item.json.is_object()) item.json = Json::object();
    Json& j = item.json;
    j["type"]      = kBalloonType;
    j["id"]        = b.id.value;
    j["view"]      = b.view.value;
    j["component"] = b.component.value;
    j["point"]     = Json::array({b.point.x, b.point.y, b.point.z});
    j["at"]        = Json::array({b.at.x, b.at.y});
}

Item MakeBalloonItem(const Balloon& b) {
    Item item;
    WriteBalloon(item, b);
    return item;
}

std::vector<Balloon> BalloonsOf(const Sheet& sheet) {
    std::vector<Balloon> out;
    for (const Item& item : sheet.annotations)
        if (auto b = ReadBalloon(item)) out.push_back(*b);
    return out;
}

Box2 AddBalloon(SheetLayout& out, const View& view, const Balloon& balloon, const std::optional<BalloonTarget>& target) {
    Box2 box;
    const bool problem = !target;
    const std::string number = target ? target->number : "?";
    const Vec2 shelf = view.at + balloon.at * view.scale.Value();
    const double w = TextWidth(number, kBalloonTextHeight) + 2;
    // The shelf runs away from the dot: to the right of a dot on its left.
    Vec2 dot = shelf;
    if (target) dot = view.at + ProjectPoint(view, target->anchor) * view.scale.Value();
    const double side = shelf.x >= dot.x ? 1.0 : -1.0;
    const Vec2 end = shelf + Vec2{w * side, 0};
    const auto line = [&](Vec2 a, Vec2 b) {
        out.polylines.push_back({{a, b}, LineWeight::Thin, LineStyle::Solid, problem});
        box.Include(a);
        box.Include(b);
    };
    line(shelf, end);
    if (target) {
        line(dot, shelf);
        Fill f;
        for (int i = 0; i < 12; ++i) {
            const double a = 2 * kPi * i / 12;
            f.points.push_back(dot + Vec2{std::cos(a), std::sin(a)} * kBalloonDot);
        }
        out.fills.push_back(std::move(f));
    }
    Label l;
    l.text    = number;
    l.height  = kBalloonTextHeight;
    l.align   = Align::Center;
    l.at      = (shelf + end) * 0.5 + Vec2{0, 1};
    l.problem = problem;
    out.labels.push_back(std::move(l));
    box.Include(shelf + Vec2{0, kBalloonTextHeight + 1});
    box.Include(end + Vec2{0, kBalloonTextHeight + 1});
    return box;
}

std::vector<Vec2> ArrangeShelves(const Box2& box, const std::vector<Vec2>& dots, double sheetWidth, double gap) {
    std::vector<Vec2> out(dots.size());
    if (dots.empty() || box.IsEmpty()) return out;
    // ГОСТ 2.109: positions in a column, outside the contour.
    const bool right = sheetWidth - box.max.x >= box.min.x;
    const double x = right ? box.max.x + 15 : box.min.x - 15;
    std::vector<std::size_t> order(dots.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return dots[a].y > dots[b].y; });
    // At the height of its dot if there is room, else under the one above it.
    double last = 1e300;
    for (std::size_t i : order) {
        const double y = std::min(dots[i].y, last - gap);
        out[i] = {x, y};
        last = y;
    }
    return out;
}

} // namespace bz::drawing
