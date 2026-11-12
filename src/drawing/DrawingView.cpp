#include "drawing/DrawingView.h"

#include <array>
#include <cmath>
#include <format>
#include <utility>

namespace bz::drawing {

namespace {

constexpr std::array<std::pair<ViewDirection, std::string_view>, 7> kDirections{{{ViewDirection::Front, "front"},
                                                                                 {ViewDirection::Back, "back"},
                                                                                 {ViewDirection::Top, "top"},
                                                                                 {ViewDirection::Bottom, "bottom"},
                                                                                 {ViewDirection::Left, "left"},
                                                                                 {ViewDirection::Right, "right"},
                                                                                 {ViewDirection::Iso, "iso"}}};

/// A number of a scale without needless zeros: 2.5 → «2,5» (ГОСТ 2.302 writes a comma).
std::string Number(double v) {
    std::string s = std::format("{:.3f}", v);
    while (s.back() == '0') s.pop_back();
    if (s.back() == '.') s.pop_back();
    for (char& c : s)
        if (c == '.') c = ',';
    return s;
}

bool ReadId(const Json& j, const char* key, ObjectId& out) {
    const auto it = j.find(key);
    if (it == j.end()) return true;   // absent: no id
    if (!it->is_number_unsigned()) return false;
    out = ObjectId{it->get<std::uint64_t>()};
    return true;
}

} // namespace

std::string_view DirectionId(ViewDirection direction) {
    for (const auto& [d, id] : kDirections)
        if (d == direction) return id;
    return "front";
}

std::optional<ViewDirection> DirectionFromId(std::string_view id) {
    for (const auto& [d, name] : kDirections)
        if (name == id) return d;
    return std::nullopt;
}

ViewBasis BasisOf(ViewDirection direction) {
    switch (direction) {
        case ViewDirection::Front:  return {{0, -1, 0}, {0, 0, 1}};
        case ViewDirection::Back:   return {{0, 1, 0}, {0, 0, 1}};
        case ViewDirection::Top:    return {{0, 0, 1}, {0, 1, 0}};
        case ViewDirection::Bottom: return {{0, 0, -1}, {0, -1, 0}};   // above the front view: its back side next to it
        case ViewDirection::Left:   return {{-1, 0, 0}, {0, 0, 1}};
        case ViewDirection::Right:  return {{1, 0, 0}, {0, 0, 1}};
        case ViewDirection::Iso: {
            // ГОСТ 2.317 isometry, from the front right above — as «Домой» shows the part.
            const Vec3 eye = Vec3{1, -1, 1}.Normalized();
            const Vec3 up = (Vec3{0, 0, 1} - eye * eye.z).Normalized();
            return {eye, up};
        }
    }
    return {{0, -1, 0}, {0, 0, 1}};
}

Vec2 ProjectPoint(ViewDirection direction, Vec3 p) {
    const ViewBasis b = BasisOf(direction);
    const Vec3 right = b.up.Cross(b.eye).Normalized();
    return {p.Dot(right), p.Dot(b.up)};
}

std::string ScaleText(Scale scale) { return Number(scale.a) + ":" + Number(scale.b); }

const std::vector<Scale>& StandardScales() {
    static const std::vector<Scale> scales{{100, 1}, {50, 1}, {40, 1}, {20, 1}, {10, 1}, {5, 1},   {4, 1},   {2.5, 1},
                                           {2, 1},   {1, 1},  {1, 2},  {1, 2.5}, {1, 4}, {1, 5},   {1, 10},  {1, 15},
                                           {1, 20},  {1, 25}, {1, 40}, {1, 50},  {1, 75}, {1, 100}, {1, 200}, {1, 400},
                                           {1, 500}, {1, 800}, {1, 1000}};
    return scales;
}

Scale FitScale(double factor) {
    for (const Scale& s : StandardScales())
        if (s.Value() <= factor * (1 + 1e-9)) return s;
    return StandardScales().back();
}

std::optional<View> ReadView(const Item& item) {
    const Json& j = item.json;
    if (!j.is_object() || item.Type() != kViewType) return std::nullopt;
    View v;
    if (!ReadId(j, "id", v.id) || !v.id || !ReadId(j, "model", v.model) || !ReadId(j, "parent", v.parent)) return std::nullopt;
    const auto dir = j.find("direction");
    if (dir == j.end() || !dir->is_string()) return std::nullopt;
    const auto direction = DirectionFromId(dir->get<std::string>());
    if (!direction) return std::nullopt;   // a direction of a newer version: kept, not shown
    v.direction = *direction;
    if (const auto s = j.find("scale"); s != j.end()) {
        if (!s->is_array() || s->size() != 2 || !(*s)[0].is_number() || !(*s)[1].is_number()) return std::nullopt;
        v.scale = {(*s)[0].get<double>(), (*s)[1].get<double>()};
        if (!(v.scale.a > 0) || !(v.scale.b > 0)) return std::nullopt;
    }
    if (const auto at = j.find("at"); at != j.end()) {
        if (!at->is_array() || at->size() != 2 || !(*at)[0].is_number() || !(*at)[1].is_number()) return std::nullopt;
        v.at = {(*at)[0].get<double>(), (*at)[1].get<double>()};
    }
    if (const auto h = j.find("hidden"); h != j.end() && h->is_boolean()) v.hidden = h->get<bool>();
    if (const auto a = j.find("axes"); a != j.end() && a->is_boolean()) v.axes = a->get<bool>();
    return v;
}

void WriteView(Item& item, const View& v) {
    if (!item.json.is_object()) item.json = Json::object();
    Json& j = item.json;
    j["type"]      = kViewType;
    j["id"]        = v.id.value;
    j["model"]     = v.model.value;
    j["direction"] = std::string(DirectionId(v.direction));
    j["scale"]     = Json::array({v.scale.a, v.scale.b});
    j["at"]        = Json::array({v.at.x, v.at.y});
    if (v.parent) j["parent"] = v.parent.value;
    else j.erase("parent");
    j["hidden"] = v.hidden;
    j["axes"]   = v.axes;
}

Item MakeViewItem(const View& view) {
    Item item;
    WriteView(item, view);
    return item;
}

bool IsKnownItem(const Item& item) { return ReadView(item).has_value(); }

std::vector<View> ViewsOf(const Sheet& sheet) {
    std::vector<View> out;
    for (const Item& item : sheet.views)
        if (auto v = ReadView(item)) out.push_back(*v);
    return out;
}

Item* FindViewItem(Sheet& sheet, ObjectId id) {
    for (Item& item : sheet.views)
        if (const auto v = ReadView(item); v && v->id == id) return &item;
    return nullptr;
}

} // namespace bz::drawing
