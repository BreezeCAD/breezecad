// Drawings (ADR-0012): the .bzd format and its compatibility rules, the
// sheet with its frame and title block (ГОСТ 2.301, 2.104).

#include "document/BzcFile.h"
#include "document/DocumentTypes.h"
#include "drawing/Drawing.h"
#include "drawing/DrawingFile.h"
#include "drawing/SheetLayout.h"

#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>

using namespace bz;
using namespace bz::drawing;
namespace fs = std::filesystem;

namespace {

fs::path Golden(const char* name) { return fs::path(BZ_SOURCE_DIR) / "tests" / "data" / "drawings" / "v1" / name; }

fs::path TempFile(const char* name) {
    const fs::path dir = fs::temp_directory_path() / "bz_drawing_tests";
    fs::create_directories(dir);
    return dir / name;
}

bool Near(Vec2 a, Vec2 b) { return std::abs(a.x - b.x) < 1e-9 && std::abs(a.y - b.y) < 1e-9; }

bool HasStroke(const SheetLayout& l, Vec2 a, Vec2 b) {
    return std::any_of(l.strokes.begin(), l.strokes.end(), [&](const Stroke& s) {
        return (Near(s.a, a) && Near(s.b, b)) || (Near(s.a, b) && Near(s.b, a));
    });
}

const Label* FindLabel(const SheetLayout& l, const std::string& text) {
    for (const Label& label : l.labels)
        if (label.text == text) return &label;
    return nullptr;
}

} // namespace

TEST_CASE("a new drawing: one A3 landscape sheet") {
    const Drawing d;
    REQUIRE(d.sheets.size() == 1);
    CHECK(d.sheets[0].name == "Лист 1");
    CHECK(d.sheets[0].size == SheetSize::A3);
    CHECK(d.sheets[0].orientation == Orientation::Landscape);
    CHECK(d.sheets[0].id.IsValid());
}

TEST_CASE("a drawing goes through drawing.json unchanged") {
    Drawing d;
    d.models.push_back({d.Ids().Next(), "Детали/Вал.bzc", Json::object()});
    Sheet& s = d.sheets[0];
    s.size = SheetSize::A2;
    s.orientation = Orientation::Portrait;
    s.title.fields[field::kName] = "Вал";
    s.title.fields[field::kDesignation] = "БРЗ.001.002";
    s.title.fields[field::kMaterial] = "";   // set empty on purpose: stays empty, not filled from the model

    ReadReport report;
    const Expected<Drawing> back = FromJson(ToJson(d), &report);
    REQUIRE(back);
    CHECK(report.warnings.empty());
    REQUIRE(back->models.size() == 1);
    CHECK(back->models[0].path == "Детали/Вал.bzc");
    CHECK(back->models[0].id == d.models[0].id);
    REQUIRE(back->sheets.size() == 1);
    CHECK(back->sheets[0].size == SheetSize::A2);
    CHECK(back->sheets[0].orientation == Orientation::Portrait);
    CHECK(back->sheets[0].title.fields.at(field::kName) == "Вал");
    CHECK(back->sheets[0].title.fields.count(field::kMaterial) == 1);
    CHECK(back->sheets[0].title.fields.at(field::kMaterial).empty());
    // Ids go on after the largest one used.
    Drawing copy = *back;
    CHECK(copy.Ids().Next().value > d.models[0].id.value);
    CHECK(ToJson(*back) == ToJson(d));
}

TEST_CASE("what a newer version wrote is kept and written back") {
    // A drawing from a future format: a view and a dimension this build does
    // not know, a new key at every level, a new title block field.
    const char* future = R"({
        "formatVersion": 7,
        "nextId": 40,
        "layers": [{"name": "Осевые"}],
        "models": [{"id": 2, "path": "Вал.bzc", "configuration": "Исп. 2"}],
        "sheets": [{
            "id": 3, "name": "Лист 1", "size": "A1", "orientation": "landscape",
            "zone": true,
            "titleBlock": {"fields": {"name": "Вал", "coating": "Хим. Окс. прм"}, "form": "2a"},
            "views": [{"id": 5, "type": "base", "model": 2, "direction": [0, -1, 0], "scale": 0.5, "at": [100, 150]}],
            "annotations": [{"id": 9, "type": "linear", "view": 5, "value": 120}]
        }]
    })";
    ReadReport report;
    const Expected<Drawing> d = FromJson(future, &report);
    REQUIRE(d);
    CHECK(report.warnings.size() == 2);   // newer version; views it does not show
    REQUIRE(d->sheets.size() == 1);
    CHECK(d->sheets[0].size == SheetSize::A1);
    REQUIRE(d->sheets[0].views.size() == 1);
    CHECK(d->sheets[0].views[0].Type() == "base");
    CHECK(d->sheets[0].title.fields.at("coating") == "Хим. Окс. прм");

    const Json saved = Json::parse(ToJson(*d));
    CHECK(saved["formatVersion"] == 7);   // still a file of the newer format
    CHECK(saved["layers"][0]["name"] == "Осевые");
    CHECK(saved["models"][0]["configuration"] == "Исп. 2");
    CHECK(saved["sheets"][0]["zone"] == true);
    CHECK(saved["sheets"][0]["titleBlock"]["form"] == "2a");
    CHECK(saved["sheets"][0]["views"][0] == Json::parse(future)["sheets"][0]["views"][0]);
    CHECK(saved["sheets"][0]["annotations"][0]["value"] == 120);
    // New ids do not collide with what the newer version used.
    Drawing copy = *d;
    CHECK(copy.Ids().Next().value >= 40);
}

TEST_CASE("a drawing.json with odd values still opens") {
    ReadReport report;
    const Expected<Drawing> d = FromJson(R"({"sheets": [{"size": "B5", "orientation": 3}, 17, {"id": "x"}]})", &report);
    REQUIRE(d);
    REQUIRE(d->sheets.size() == 2);   // the number is not a sheet
    CHECK(d->sheets[0].size == SheetSize::A3);
    CHECK(d->sheets[0].id.IsValid());
    CHECK(d->sheets[1].id.IsValid());
    CHECK(d->sheets[0].id != d->sheets[1].id);
    CHECK_FALSE(report.warnings.empty());   // «Неизвестный формат листа «B5»»

    const Expected<Drawing> empty = FromJson("{}");
    REQUIRE(empty);
    CHECK(empty->sheets.size() == 1);   // a drawing always has a sheet
    CHECK_FALSE(FromJson("not json"));
}

TEST_CASE("a .bzd is the shared container with the drawing kind") {
    Drawing d;
    d.sheets[0].title.fields[field::kName] = "Корпус";
    const fs::path file = TempFile("Корпус.bzd");
    REQUIRE(SaveDrawing(d, file, "BreezeCAD test"));
    const Expected<std::string> kind = doc::ReadDocumentType(file);
    REQUIRE(kind);
    CHECK(*kind == doc::kDrawingType);
    const Expected<Drawing> back = LoadDrawing(file);
    REQUIRE(back);
    CHECK(back->sheets[0].title.fields.at(field::kName) == "Корпус");

    // A part is not a drawing.
    const fs::path part = TempFile("Деталь.bzc");
    REQUIRE(doc::SaveBzc(doc::Document{}, part, "BreezeCAD test"));
    CHECK_FALSE(LoadDrawing(part));
    fs::remove_all(file.parent_path());
}

TEST_CASE("golden files of format 1 open forever") {
    // tests/data/drawings/v1: files written by the first format. Never delete
    // or rewrite them: every later version must open them (ADR-0012).
    ReadReport report;
    const Expected<Drawing> d = LoadDrawing(Golden("a3-vala.bzd"), &report);
    if (!d) FAIL(d.GetError().message);
    CHECK(report.warnings.empty());
    REQUIRE(d->models.size() == 1);
    CHECK(d->models[0].path == "Вал.bzc");
    REQUIRE(d->sheets.size() == 2);
    CHECK(d->sheets[0].size == SheetSize::A3);
    CHECK(d->sheets[0].orientation == Orientation::Landscape);
    CHECK(d->sheets[0].title.fields.at(field::kName) == "Вал");
    CHECK(d->sheets[0].title.fields.at(field::kDesignation) == "БРЗ.001.002");
    CHECK(d->sheets[0].title.fields.at(field::kMaterial) == "Сталь 45 ГОСТ 1050-2013");
    CHECK(d->sheets[1].name == "Лист 2");
    CHECK(d->sheets[1].size == SheetSize::A4);
}

TEST_CASE("sheet sizes of ГОСТ 2.301; A4 is upright") {
    CHECK(Near(SheetSizeMm(SheetSize::A3, Orientation::Landscape), {420, 297}));
    CHECK(Near(SheetSizeMm(SheetSize::A3, Orientation::Portrait), {297, 420}));
    CHECK(Near(SheetSizeMm(SheetSize::A0, Orientation::Landscape), {1189, 841}));
    CHECK(Near(SheetSizeMm(SheetSize::A1, Orientation::Landscape), {841, 594}));
    CHECK(Near(SheetSizeMm(SheetSize::A2, Orientation::Portrait), {420, 594}));
    CHECK(Near(SheetSizeMm(SheetSize::A4, Orientation::Landscape), {210, 297}));
}

TEST_CASE("the frame and the title block of form 1") {
    Sheet s;
    s.size = SheetSize::A3;
    s.title.fields[field::kName] = "Вал";
    s.title.fields[field::kDesignation] = "БРЗ.001.002";
    const SheetLayout l = LayoutSheet(s, 1, 3);
    CHECK(l.width == 420);
    CHECK(l.height == 297);
    // The frame: 20 mm from the left edge, 5 from the others.
    CHECK(Near(l.frame.min, {20, 5}));
    CHECK(Near(l.frame.max, {415, 292}));
    // The title block: 185 × 55 in the lower right corner of the frame.
    CHECK(Near(l.titleBlock.min, {230, 5}));
    CHECK(Near(l.titleBlock.max, {415, 60}));
    CHECK(HasStroke(l, {230, 5}, {230, 60}));     // its left edge
    CHECK(HasStroke(l, {230, 60}, {415, 60}));    // its top edge
    CHECK(HasStroke(l, {295, 45}, {415, 45}));    // under graph 2 (designation, 120 × 15)
    CHECK(HasStroke(l, {365, 5}, {365, 45}));     // graphs 4–9: 50 mm on the right
    CHECK(HasStroke(l, {295, 5}, {295, 60}));     // the signatures part: 65 mm on the left
    // Fields and numbers in their graphs.
    const Label* name = FindLabel(l, "Вал");
    REQUIRE(name);
    CHECK(name->field == field::kName);
    CHECK(name->height == 7);
    CHECK(name->at.x == doctest::Approx(330));   // the middle of graph 1 (295…365)
    CHECK(FindLabel(l, "Разраб."));
    CHECK(FindLabel(l, "Масштаб"));
    CHECK(FindLabel(l, "Формат A3"));
    REQUIRE(FindLabel(l, "3"));   // Листов
    // The designation twice: graph 2 and graph 26 (top left, turned over).
    const auto designations = std::count_if(l.labels.begin(), l.labels.end(), [](const Label& x) { return x.text == "БРЗ.001.002"; });
    CHECK(designations == 2);
    // Every line on the sheet.
    for (const Stroke& st : l.strokes) {
        CHECK(st.a.x >= 0);
        CHECK(st.b.x <= l.width);
        CHECK(st.a.y >= 0);
        CHECK(st.b.y <= l.height);
    }
}
