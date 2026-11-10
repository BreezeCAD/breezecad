#include "drawing/ModelView.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <utility>

namespace bz::drawing {

namespace {

constexpr std::array<std::pair<ViewProjection, std::string_view>, 3> kProjections{{
    {ViewProjection::Front, "front"}, {ViewProjection::Top, "top"}, {ViewProjection::Left, "left"}}};

// ГОСТ 2.302-68, п. 2: масштабы увеличения, натуральная величина, масштабы уменьшения.
constexpr std::array<Scale, 27> kScales{{
    {100, 1}, {50, 1}, {40, 1}, {20, 1}, {10, 1}, {5, 1}, {4, 1}, {2.5, 1}, {2, 1},
    {1, 1},
    {1, 2}, {1, 2.5}, {1, 4}, {1, 5}, {1, 10}, {1, 15}, {1, 20}, {1, 25}, {1, 40}, {1, 50}, {1, 75}, {1, 100},
    {1, 200}, {1, 400}, {1, 500}, {1, 800}, {1, 1000}}};

std::optional<ObjectId> IdAt(const Json& obj, const char* key) {
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_number_unsigned()) return std::nullopt;
    return ObjectId{it->get<std::uint64_t>()};
}

std::optional<Vec2> PairAt(const Json& obj, const char* key) {
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_array() || it->size() != 2 || !(*it)[0].is_number() || !(*it)[1].is_number()) return std::nullopt;
    const Vec2 v{(*it)[0].get<double>(), (*it)[1].get<double>()};
    if (!std::isfinite(v.x) || !std::isfinite(v.y)) return std::nullopt;
    return v;
}

/// 2.5 as "2,5", 2 as "2": the decimal comma of the standard.
std::string Number(double v) {
    std::string text = std::format("{}", v);
    std::replace(text.begin(), text.end(), '.', ',');
    return text;
}

} // namespace

std::string ScaleText(Scale scale) { return std::format("{}:{}", Number(scale.image), Number(scale.object)); }

std::span<const Scale> StandardScales() { return kScales; }

std::string_view ProjectionName(ViewProjection projection) {
    switch (projection) {
        case ViewProjection::Front: return "Главный вид";
        case ViewProjection::Top:   return "Вид сверху";
        case ViewProjection::Left:  return "Вид слева";
    }
    return "Вид";
}

std::optional<ModelView> ReadModelView(const Item& item) {
    if (!item.json.is_object() || item.Type() != kModelViewType) return std::nullopt;
    const Json& j = item.json;
    ModelView v;
    const auto id = IdAt(j, "id"), model = IdAt(j, "model");
    if (!id || !id->IsValid() || !model) return std::nullopt;   // nothing can refer to it: leave it as it is
    v.id    = *id;
    v.model = *model;
    const auto projection = j.find("projection");
    if (projection == j.end() || !projection->is_string()) return std::nullopt;
    const std::string name = projection->get<std::string>();
    const auto known = std::find_if(kProjections.begin(), kProjections.end(), [&](const auto& p) { return p.second == name; });
    if (known == kProjections.end()) return std::nullopt;   // a projection of a newer version
    v.projection = known->first;
    if (const auto scale = PairAt(j, "scale"); scale && scale->x > 0 && scale->y > 0) v.scale = Scale{scale->x, scale->y};
    v.at = PairAt(j, "at").value_or(Vec2{});
    if (const auto parent = IdAt(j, "parent")) v.parent = *parent;
    if (const auto hidden = j.find("hiddenLines"); hidden != j.end() && hidden->is_boolean()) v.hiddenLines = hidden->get<bool>();
    return v;
}

bool IsModelView(const Item& item) { return ReadModelView(item).has_value(); }

void StoreModelView(Item& item, const ModelView& view) {
    if (!item.json.is_object()) item.json = Json::object();
    Json& j = item.json;
    j["type"]  = kModelViewType;
    j["id"]    = view.id.value;
    j["model"] = view.model.value;
    for (const auto& [projection, name] : kProjections)
        if (projection == view.projection) j["projection"] = std::string(name);
    if (view.scale) j["scale"] = Json::array({view.scale->image, view.scale->object});
    else j.erase("scale");
    j["at"] = Json::array({view.at.x, view.at.y});
    if (view.parent.IsValid()) j["parent"] = view.parent.value;
    else j.erase("parent");
    j["hiddenLines"] = view.hiddenLines;
}

Item MakeViewItem(const ModelView& view) {
    Item item{Json::object()};
    StoreModelView(item, view);
    return item;
}

std::vector<ModelView> ModelViewsOf(const Sheet& sheet) {
    std::vector<ModelView> views;
    for (const Item& item : sheet.views)
        if (auto view = ReadModelView(item)) views.push_back(*view);
    return views;
}

const Item* FindViewItem(const Sheet& sheet, ObjectId id) {
    for (const Item& item : sheet.views)
        if (const auto view = ReadModelView(item); view && view->id == id) return &item;
    return nullptr;
}

Item* FindViewItem(Sheet& sheet, ObjectId id) { return const_cast<Item*>(FindViewItem(std::as_const(sheet), id)); }

std::size_t UnknownItemCount(const Sheet& sheet) {
    const auto unknown = std::count_if(sheet.views.begin(), sheet.views.end(), [](const Item& item) { return !IsModelView(item); });
    return static_cast<std::size_t>(unknown) + sheet.annotations.size();
}

std::size_t UnknownItemCount(const Drawing& drawing) {
    std::size_t count = 0;
    for (const Sheet& sheet : drawing.sheets) count += UnknownItemCount(sheet);
    return count;
}

} // namespace bz::drawing
