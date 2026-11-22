// Dimensions of a drawing (ADR-0012, step 5): the item in the file, what
// they measure in a view, their text, and how they are laid out on the sheet.
#include "drawing/Dimension.h"
#include "drawing/Drawing.h"
#include "drawing/DrawingFile.h"
#include "drawing/SheetLayout.h"

#include <doctest.h>

#include <filesystem>

using namespace bz;
using namespace bz::drawing;

namespace {

ModelEdge Line(Vec3 a, Vec3 b) {
    ModelEdge e;
    e.kind = ModelEdge::Kind::Line;
    e.a = a;
    e.b = b;
    return e;
}

ModelEdge Circle(Vec3 center, Vec3 axis, double r) {
    ModelEdge e;
    e.kind = ModelEdge::Kind::Circle;
    e.center = center;
    e.axis = axis;
    e.radius = r;
    e.a = e.b = center + Vec3{r, 0, 0};
    return e;
}

Json Ref(Vec3 middle) { return Json{{"curve", "line"}, {"middle", Json::array({middle.x, middle.y, middle.z})}}; }

View Front(Scale scale = {1, 1}) {
    View v;
    v.id = ObjectId{7};
    v.model = ObjectId{2};
    v.direction = ViewDirection::Front;
    v.scale = scale;
    v.at = {100, 100};
    return v;
}

} // namespace

TEST_CASE("dimension: written and read back; unknown keys and kinds stay") {
    Dimension d;
    d.id = ObjectId{9};
    d.view = ObjectId{7};
    d.kind = DimensionKind::Linear;
    d.axis = DimensionAxis::Horizontal;
    d.edges = {Ref({0, 0, 30}), Ref({0, 0, 0})};
    d.at = {40, 25};
    d.text = "<> h14";
    Item item = MakeDimensionItem(d);
    item.json["future"] = 1;
    const auto back = ReadDimension(item);
    REQUIRE(back);
    CHECK(back->axis == DimensionAxis::Horizontal);
    CHECK(back->edges.size() == 2);
    CHECK(back->at == Vec2{40, 25});
    CHECK(back->text == "<> h14");
    WriteDimension(item, *back);
    CHECK(item.json["future"] == 1);
    CHECK(IsKnownItem(item));

    Item newer;
    newer.json = {{"type", "dimension"}, {"id", 3}, {"view", 7}, {"kind", "arcLength"}, {"edges", Json::array({Ref({})})}};
    CHECK_FALSE(ReadDimension(newer));   // a kind of a newer version: kept, not shown
    CHECK_FALSE(IsKnownItem(newer));
}

TEST_CASE("dimension: a drawing with dimensions opens without the «newer» warning; an unknown one warns") {
    Drawing dr;
    Dimension d;
    d.id = dr.Ids().Next();
    d.view = ObjectId{7};
    d.edges = {Ref({0, 0, 0})};
    dr.sheets[0].annotations.push_back(MakeDimensionItem(d));
    ReadReport report;
    auto back = FromJson(ToJson(dr), &report);
    REQUIRE(back);
    CHECK(report.warnings.empty());
    CHECK(DimensionsOf(back->sheets[0]).size() == 1);

    Item note;
    note.json = {{"type", "roughness"}, {"id", 77}};
    back->sheets[0].annotations.push_back(note);
    ReadReport again;
    REQUIRE(FromJson(ToJson(*back), &again));
    CHECK(again.warnings.size() == 1);
}

TEST_CASE("dimension: linear — an edge's length, two edges apart, horizontal and vertical") {
    const View v = Front({1, 2});
    Dimension d;
    d.view = v.id;
    d.edges = {Ref({})};
    d.at = {0, 50};
    // The top edge of a 120 mm plate: 120 whatever the scale of the view.
    auto m = MeasureOf(v, d, {Line({-60, 0, 30}, {60, 0, 30})});
    REQUIRE(m);
    CHECK(m->value == doctest::Approx(120));
    CHECK(ValueText(*m) == "120");
    CHECK((m->p2 - m->p1).Length() == doctest::Approx(60));   // on the sheet at 1:2

    // Two parallel edges, aligned: the distance between them across.
    d.edges = {Ref({}), Ref({})};
    m = MeasureOf(v, d, {Line({-60, 0, 30}, {60, 0, 30}), Line({-60, 0, 0}, {60, 0, 0})});
    REQUIRE(m);
    CHECK(m->value == doctest::Approx(30));

    // A hole's centre and an edge, horizontally and vertically.
    d.axis = DimensionAxis::Horizontal;
    m = MeasureOf(v, d, {Circle({-35.5, 0, 15}, {0, 1, 0}, 5), Line({-60, 0, 0}, {-60, 0, 30})});
    REQUIRE(m);
    CHECK(m->value == doctest::Approx(24.5));
    CHECK(ValueText(*m) == "24,5");
    d.axis = DimensionAxis::Vertical;
    m = MeasureOf(v, d, {Circle({-35.5, 0, 15}, {0, 1, 0}, 5), Line({-60, 0, 0}, {60, 0, 0})});
    REQUIRE(m);
    CHECK(m->value == doctest::Approx(15));
}

TEST_CASE("dimension: diameter and radius, end on and from the side") {
    const View v = Front();
    Dimension d;
    d.view = v.id;
    d.kind = DimensionKind::Diameter;
    d.edges = {Ref({})};
    d.at = {30, 30};
    auto m = MeasureOf(v, d, {Circle({0, 0, 15}, {0, 1, 0}, 5)});   // a hole seen end on in the front view
    REQUIRE(m);
    CHECK(m->endOn);
    CHECK(DimensionText(d, *m) == "Ø10");
    m = MeasureOf(v, d, {Circle({0, 0, 30}, {0, 0, 1}, 5)});   // its rim from the side: a line 10 long
    REQUIRE(m);
    CHECK_FALSE(m->endOn);
    CHECK((m->p2 - m->p1).Length() == doctest::Approx(10));
    d.kind = DimensionKind::Radius;
    m = MeasureOf(v, d, {Circle({0, 0, 15}, {0, -1, 0}, 2.5)});
    REQUIRE(m);
    CHECK(DimensionText(d, *m) == "R2,5");
    std::string why;
    CHECK_FALSE(MeasureOf(v, d, {Line({0, 0, 0}, {1, 0, 0})}, &why));
    CHECK_FALSE(why.empty());
}

TEST_CASE("dimension: angle — the sector the line is in; parallel edges make none") {
    const View v = Front();
    Dimension d;
    d.view = v.id;
    d.kind = DimensionKind::Angle;
    d.edges = {Ref({}), Ref({})};
    d.at = {20, 5};   // between the positive x axis and the 30° line
    const ModelEdge base = Line({0, 0, 0}, {50, 0, 0});
    const ModelEdge slope = Line({0, 0, 0}, {50 * std::cos(kPi / 6), 0, 50 * std::sin(kPi / 6)});
    auto m = MeasureOf(v, d, {base, slope});
    REQUIRE(m);
    CHECK(m->value == doctest::Approx(30));
    CHECK(ValueText(*m) == "30°");
    d.at = {-20, 5};   // on the other side: 150°
    m = MeasureOf(v, d, {base, slope});
    REQUIRE(m);
    CHECK(m->value == doctest::Approx(150));
    m->value = 12.5;
    CHECK(ValueText(*m) == "12°30'");
    CHECK_FALSE(MeasureOf(v, d, {base, Line({0, 0, 10}, {50, 0, 10})}));
}

TEST_CASE("dimension: on the sheet — lines, two arrows, the text; a lost one is red") {
    const View v = Front();
    Dimension d;
    d.id = ObjectId{9};
    d.view = v.id;
    d.axis = DimensionAxis::Horizontal;
    d.edges = {Ref({})};
    d.at = {0, 45};
    const auto m = MeasureOf(v, d, {Line({-60, 0, 30}, {60, 0, 30})});
    SheetLayout l;
    const Box2 box = AddDimension(l, v, d, m);
    CHECK(l.fills.size() == 2);
    REQUIRE(l.labels.size() == 1);
    CHECK(l.labels[0].text == "120");
    CHECK(l.labels[0].height == doctest::Approx(kDimTextHeight));
    CHECK(l.labels[0].at.y > 145);   // over the dimension line at y = 100 + 45
    CHECK(box.min.x == doctest::Approx(40).epsilon(1e-3));
    CHECK(box.max.y > 147);

    SheetLayout lost;
    AddDimension(lost, v, d, std::nullopt, {{100, 130}});
    REQUIRE(lost.labels.size() == 1);
    CHECK(lost.labels[0].problem);
    CHECK(lost.labels[0].text == "?");
    CHECK(lost.polylines.size() == 1);
    CHECK(lost.polylines[0].problem);
}

TEST_CASE("dimension: laid out on the page with the view; it follows the edges the content gives") {
    Drawing dr;
    Sheet& s = dr.sheets[0];
    View v = Front();
    s.views.push_back(MakeViewItem(v));
    Dimension d;
    d.id = ObjectId{9};
    d.view = v.id;
    d.edges = {Ref({0, 0, 30})};
    d.at = {0, 45};
    s.annotations.push_back(MakeDimensionItem(d));
    double length = 120;
    SheetContent content;
    content.edges = [&](const Sheet&, const View&, const std::vector<Json>& refs) {
        return std::vector<std::optional<ModelEdge>>(refs.size(), Line({-length / 2, 0, 30}, {length / 2, 0, 30}));
    };
    const std::vector<Page> pages = PagesOf(dr, content);
    REQUIRE(pages.size() == 1);
    SheetLayout l = LayoutPage(dr, pages[0], content);
    REQUIRE(l.hits.size() == 1);
    CHECK(l.hits[0].id == ObjectId{9});
    const auto valueOf = [](const SheetLayout& x) { return x.labels.empty() ? std::string{} : x.labels.back().text; };   // dimensions come last
    CHECK(valueOf(l) == "120");
    length = 150;   // the part changed
    l = LayoutPage(dr, pages[0], content);
    CHECK(valueOf(l) == "150");
}

TEST_CASE("golden v1 with dimensions: read, then written back the same") {
    const auto path = std::filesystem::path(BZ_SOURCE_DIR) / "tests" / "data" / "drawings" / "v1" / "a3-dimensions.bzd";
    ReadReport report;
    const auto d = LoadDrawing(path, &report);
    REQUIRE(d);
    CHECK(report.warnings.empty());
    const std::vector<Dimension> dims = DimensionsOf(d->sheets.front());
    REQUIRE(dims.size() == 3);
    CHECK(dims[0].kind == DimensionKind::Linear);
    CHECK(dims[0].axis == DimensionAxis::Horizontal);
    CHECK(dims[1].kind == DimensionKind::Diameter);
    CHECK(dims[2].kind == DimensionKind::Angle);
    CHECK(dims[2].edges.size() == 2);
    const auto again = FromJson(ToJson(*d));
    REQUIRE(again);
    CHECK(ToJson(*again) == ToJson(*d));
}
