#include "drawing/Drawing.h"

#include "drawing/DrawingView.h"

#include <algorithm>
#include <array>
#include <format>
#include <initializer_list>
#include <utility>

namespace bz::drawing {

namespace {

constexpr std::array<std::pair<SheetSize, std::string_view>, 5> kSizes{{
    {SheetSize::A4, "A4"}, {SheetSize::A3, "A3"}, {SheetSize::A2, "A2"}, {SheetSize::A1, "A1"}, {SheetSize::A0, "A0"}}};
constexpr std::array<std::pair<Orientation, std::string_view>, 2> kOrientations{{
    {Orientation::Portrait, "portrait"}, {Orientation::Landscape, "landscape"}}};

/// `obj` without the keys this version reads: what a newer one added.
Json Rest(const Json& obj, std::initializer_list<const char*> known) {
    Json rest = Json::object();
    for (auto it = obj.begin(); it != obj.end(); ++it)
        if (std::none_of(known.begin(), known.end(), [&](const char* k) { return it.key() == k; })) rest[it.key()] = it.value();
    return rest;
}

/// `extra` put back into `obj`: what this version knows wins on a clash.
void PutBack(Json& obj, const Json& extra) {
    if (!extra.is_object()) return;
    for (auto it = extra.begin(); it != extra.end(); ++it)
        if (!obj.contains(it.key())) obj[it.key()] = it.value();
}

std::string Text(const Json& obj, const char* key, std::string fallback = {}) {
    const auto it = obj.find(key);
    return it != obj.end() && it->is_string() ? it->get<std::string>() : fallback;
}

ObjectId IdOf(const Json& obj, IdGenerator& ids) {
    const auto it = obj.find("id");
    if (it != obj.end() && it->is_number_unsigned()) {
        const ObjectId id{it->get<std::uint64_t>()};
        ids.Reserve(id);
        if (id.IsValid() && id.value <= IdGenerator::kLargest) return id;
    }
    return ids.Next();   // missing or damaged: a fresh one, nothing refers to it yet
}

template <class T, std::size_t N>
T FromId(const std::array<std::pair<T, std::string_view>, N>& table, const std::string& id, T fallback, std::vector<std::string>* warnings,
         const char* what) {
    for (const auto& [value, name] : table)
        if (name == id) return value;
    if (!id.empty() && warnings) warnings->push_back(std::format("Неизвестный {} «{}» — взят {}", what, id, std::string(table[0].second)));
    return fallback;
}

std::vector<Item> ItemsFrom(const Json& obj, const char* key) {
    std::vector<Item> items;
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_array()) return items;
    for (const Json& j : *it)
        if (j.is_object()) items.push_back({j});   // kept whole: this version has no types of its own yet
    return items;
}

Json ItemsTo(const std::vector<Item>& items) {
    Json out = Json::array();
    for (const Item& item : items) out.push_back(item.json);
    return out;
}

} // namespace

std::string Item::Type() const { return Text(json, "type"); }

std::string_view SizeId(SheetSize size) {
    for (const auto& [value, name] : kSizes)
        if (value == size) return name;
    return "A3";
}

std::string_view OrientationId(Orientation orientation) {
    return orientation == Orientation::Portrait ? "portrait" : "landscape";
}

Drawing::Drawing() {
    Sheet first;
    first.id   = myIds.Next();
    first.name = "Лист 1";
    sheets.push_back(std::move(first));
}

std::string ToJson(const Drawing& d) {
    Json models = Json::array();
    for (const ModelRef& m : d.models) {
        Json j{{"id", m.id.value}, {"path", m.path}};
        PutBack(j, m.extra);
        models.push_back(std::move(j));
    }
    Json sheets = Json::array();
    for (const Sheet& s : d.sheets) {
        Json title = Json::object();
        for (const auto& [key, value] : s.title.fields) title[key] = value;
        Json titleBlock{{"fields", std::move(title)}};
        PutBack(titleBlock, s.title.extra);
        Json j{{"id", s.id.value},
               {"name", s.name},
               {"size", std::string(SizeId(s.size))},
               {"orientation", std::string(OrientationId(s.orientation))},
               {"titleBlock", std::move(titleBlock)},
               {"views", ItemsTo(s.views)},
               {"annotations", ItemsTo(s.annotations)}};
        if (s.kind == SheetKind::Specification) j["kind"] = "specification";
        if (s.model) j["model"] = s.model.value;
        PutBack(j, s.extra);
        sheets.push_back(std::move(j));
    }
    // A file of a newer version stays one: what this build did not understand is in it still.
    int version = kDrawingFormatVersion;
    if (const auto v = d.extra.find("formatVersion"); v != d.extra.end() && v->is_number_integer()) version = std::max(version, v->get<int>());
    Json root{{"formatVersion", version},
              {"nextId", d.Ids().Last() + 1},
              {"models", std::move(models)},
              {"sheets", std::move(sheets)}};
    PutBack(root, d.extra);
    return root.dump(1, '\t');
}

Expected<Json> Upgrade(Json root, int version) {
    if (version < 1) version = 1;   // a file without the key: the first format
    // When the format changes: `if (version == 1) { …move v1 to v2…; version = 2; }` — one step
    // each, in order, with a test that reads a v1 file and checks the result.
    if (version > kDrawingFormatVersion) return root;   // newer: read as it is, the caller warns
    return root;
}

Expected<Drawing> FromJson(std::string_view text, ReadReport* report) {
    Json root = Json::parse(text, nullptr, false);
    if (!root.is_object()) return Error{"Содержимое чертежа повреждено: это не JSON"};
    std::vector<std::string>* warnings = report ? &report->warnings : nullptr;
    int version = 1;
    if (const auto v = root.find("formatVersion"); v != root.end() && v->is_number_integer()) version = v->get<int>();
    if (version > kDrawingFormatVersion && warnings)
        warnings->push_back("Чертёж создан более новой версией BreezeCAD. Новые возможности не покажутся, но при сохранении не пропадут.");
    Expected<Json> upgraded = Upgrade(std::move(root), version);
    if (!upgraded) return upgraded.GetError();
    const Json& r = *upgraded;

    Drawing d;
    d.sheets.clear();
    d.extra = Rest(r, {"formatVersion", "nextId", "models", "sheets"});
    // A newer file keeps its version number: saving here must not claim the old format.
    if (version > kDrawingFormatVersion) d.extra["formatVersion"] = version;
    if (const auto next = r.find("nextId"); next != r.end() && next->is_number_unsigned() && next->get<std::uint64_t>() > 0)
        d.Ids().Reserve(ObjectId{next->get<std::uint64_t>() - 1});

    if (const auto models = r.find("models"); models != r.end() && models->is_array())
        for (const Json& m : *models) {
            if (!m.is_object()) continue;
            ModelRef ref;
            ref.id    = IdOf(m, d.Ids());
            ref.path  = Text(m, "path");
            ref.extra = Rest(m, {"id", "path"});
            d.models.push_back(std::move(ref));
        }

    if (const auto sheets = r.find("sheets"); sheets != r.end() && sheets->is_array())
        for (const Json& s : *sheets) {
            if (!s.is_object()) continue;
            Sheet sheet;
            sheet.id          = IdOf(s, d.Ids());
            sheet.name        = Text(s, "name", std::format("Лист {}", d.sheets.size() + 1));
            sheet.size        = FromId(kSizes, Text(s, "size"), SheetSize::A3, warnings, "формат листа");
            sheet.orientation = FromId(kOrientations, Text(s, "orientation"), Orientation::Landscape, warnings, "поворот листа");
            if (const auto tb = s.find("titleBlock"); tb != s.end() && tb->is_object()) {
                if (const auto fields = tb->find("fields"); fields != tb->end() && fields->is_object())
                    for (auto f = fields->begin(); f != fields->end(); ++f)
                        if (f.value().is_string()) sheet.title.fields[f.key()] = f.value().get<std::string>();
                sheet.title.extra = Rest(*tb, {"fields"});
            }
            sheet.views       = ItemsFrom(s, "views");
            sheet.annotations = ItemsFrom(s, "annotations");
            if (Text(s, "kind") == "specification") sheet.kind = SheetKind::Specification;
            if (const auto m = s.find("model"); m != s.end() && m->is_number_unsigned()) sheet.model = ObjectId{m->get<std::uint64_t>()};
            // A kind of a newer version stays in `extra` and is written back; the sheet is shown as a drawing.
            sheet.extra = Rest(s, {"id", "name", "size", "orientation", "titleBlock", "views", "annotations", "model"});
            if (sheet.kind == SheetKind::Specification) sheet.extra.erase("kind");
            d.sheets.push_back(std::move(sheet));
        }
    if (d.sheets.empty()) {   // a drawing always has a sheet to show
        Sheet first;
        first.id   = d.Ids().Next();
        first.name = "Лист 1";
        d.sheets.push_back(std::move(first));
    }
    const bool unknown = std::any_of(d.sheets.begin(), d.sheets.end(), [](const Sheet& s) {
        return !s.annotations.empty() || std::any_of(s.views.begin(), s.views.end(), [](const Item& i) { return !IsKnownItem(i); });
    });
    if (unknown && warnings)
        warnings->push_back("В чертеже есть виды или обозначения, которые эта версия не показывает. Они сохранятся без изменений.");
    return d;
}

} // namespace bz::drawing
