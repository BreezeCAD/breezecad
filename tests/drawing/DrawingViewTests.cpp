// Views of a drawing (ADR-0012, step 3): the item in the file, scales of
// ГОСТ 2.302, and the lines of a view on the sheet.
#include "drawing/Drawing.h"
#include "drawing/DrawingView.h"
#include "drawing/SheetLayout.h"

#include <doctest.h>

#include <filesystem>

using namespace bz;
using namespace bz::drawing;

TEST_CASE("view: written and read back; unknown keys and types stay") {
    View v;
    v.id = ObjectId{7};
    v.model = ObjectId{2};
    v.direction = ViewDirection::Left;
    v.scale = {1, 2.5};
    v.at = {120.5, 180};
    v.parent = ObjectId{5};
    v.hidden = false;
    Item item = MakeViewItem(v);
    item.json["future"] = "keep me";
    const auto back = ReadView(item);
    REQUIRE(back);
    CHECK(back->direction == ViewDirection::Left);
    CHECK(back->scale == Scale{1, 2.5});
    CHECK(back->at == Vec2{120.5, 180});
    CHECK(back->parent == ObjectId{5});
    CHECK_FALSE(back->hidden);
    CHECK(back->axes);

    // Changed and written again: the key of a newer version is still there.
    View moved = *back;
    moved.at = {10, 20};
    moved.parent = {};
    WriteView(item, moved);
    CHECK(item.json["future"] == "keep me");
    CHECK_FALSE(item.json.contains("parent"));

    // A direction or a type this version does not know: not a view it shows, kept as is.
    Item newer = item;
    newer.json["direction"] = "auxiliary";
    CHECK_FALSE(ReadView(newer));
    CHECK_FALSE(IsKnownItem(Item{Json{{"type", "section"}, {"id", 9}}}));
    CHECK_FALSE(ReadView(Item{Json{{"type", "view"}, {"direction", "front"}}}));   // no id
}

TEST_CASE("view: a drawing with views reads without the «newer version» warning") {
    Drawing d;
    View v;
    v.id = d.Ids().Next();
    d.sheets.front().views.push_back(MakeViewItem(v));
    ReadReport report;
    auto back = FromJson(ToJson(d), &report);
    REQUIRE(back);
    CHECK(report.warnings.empty());
    CHECK(ViewsOf(back->sheets.front()).size() == 1);

    back->sheets.front().views.push_back(Item{Json{{"type", "section"}, {"id", 99}}});
    ReadReport again;
    REQUIRE(FromJson(ToJson(*back), &again));
    CHECK(again.warnings.size() == 1);
}

TEST_CASE("scales of ГОСТ 2.302") {
    CHECK(ScaleText({1, 2}) == "1:2");
    CHECK(ScaleText({2.5, 1}) == "2,5:1");
    CHECK(ScaleText({1, 1}) == "1:1");
    CHECK(FitScale(1.0) == Scale{1, 1});
    CHECK(FitScale(0.45) == Scale{1, 2.5});
    CHECK(FitScale(3.9) == Scale{2.5, 1});
    CHECK(FitScale(1e-9) == StandardScales().back());
    CHECK(FitScale(1e9) == StandardScales().front());
}

TEST_CASE("view lines on the sheet: scaled, placed, styled") {
    ViewLines lines;
    lines.visible = {{{0, 0}, {100, 0}}};
    lines.hidden = {{{10, 0}, {10, 30}}};
    lines.marks = {{{50, 20}, 20}, {{0, 0}, 1}};   // Ø40 → 20 mm on paper; Ø2 → 1 mm
    lines.axes = {{{0, 10}, {100, 10}}};
    View v;
    v.scale = {1, 2};
    v.at = {200, 100};
    SheetLayout l;
    AddView(l, v, lines);
    // visible, hidden, 2 × 2 cross lines, an axis
    REQUIRE(l.polylines.size() == 7);
    const Polyline* visible = nullptr;
    for (const Polyline& p : l.polylines)
        if (p.weight == LineWeight::Thick) visible = &p;
    REQUIRE(visible);
    CHECK(visible->points[1] == Vec2{250, 100});
    int dashed = 0, dashDot = 0, solidThin = 0;
    for (const Polyline& p : l.polylines) {
        if (p.style == LineStyle::Dashed) ++dashed;
        if (p.style == LineStyle::DashDot) ++dashDot;
        if (p.style == LineStyle::Solid && p.weight == LineWeight::Thin) ++solidThin;
    }
    CHECK(dashed == 1);
    CHECK(dashDot == 3);     // the big circle's cross and the axis
    CHECK(solidThin == 2);   // Ø1 at 1:2 is under 12 mm: solid thin lines

    // Centre lines go 3 mm past the contour on paper.
    const Box2 box = ViewBox(v, lines);
    CHECK(box.min.x == doctest::Approx(196.5));   // the small cross at the origin: 0.5 + 3
    CHECK(box.max.x == doctest::Approx(253));

    v.hidden = false;
    v.axes = false;
    SheetLayout bare;
    AddView(bare, v, lines);
    CHECK(bare.polylines.size() == 1);
}

#include "drawing/ViewPlacement.h"

namespace {

Box3 Model(Vec3 lo, Vec3 hi) {
    Box3 b;
    b.Include(lo);
    b.Include(hi);
    return b;
}

Box2 Placed(const View& v, const Box3& m) {
    const Box2 e = Extent(v.direction, m);
    Box2 b;
    b.Include(v.at + e.min * v.scale.Value());
    b.Include(v.at + e.max * v.scale.Value());
    return b;
}

} // namespace

TEST_CASE("standard views: in projection, on the sheet, off the title block") {
    Drawing d;   // A3 landscape
    const SheetLayout sheet = LayoutSheet(d.sheets.front(), 1, 1);
    const Room room = RoomOf(sheet);
    const Box3 bracket = Model({-60, -40, 0}, {60, 40, 45});
    const auto p = PlaceStandardViews(room, bracket);
    REQUIRE(p);
    CHECK(p->scale == Scale{1, 1});   // 120 + 20 + 80 wide, 45 + 20 + 80 high: fits A3 at 1:1, not at 2:1
    CHECK(p->top.x == doctest::Approx(p->front.x));    // the top view under the front one
    CHECK(p->left.y == doctest::Approx(p->front.y));   // the left one level with it
    View front, top, left;
    front.at = p->front;
    top.direction = ViewDirection::Top;
    top.at = p->top;
    left.direction = ViewDirection::Left;
    left.at = p->left;
    for (const View& v : {front, top, left}) CHECK(room.Fits(Placed(v, bracket)));
    CHECK(Placed(top, bracket).max.y < Placed(front, bracket).min.y);    // under
    CHECK(Placed(left, bracket).min.x > Placed(front, bracket).max.x);   // to the right

    // A long shaft on A4: a smaller standard scale.
    Sheet a4;
    a4.size = SheetSize::A4;
    const auto shaft = PlaceStandardViews(RoomOf(LayoutSheet(a4, 1, 1)), Model({0, -20, -20}, {600, 20, 20}));
    REQUIRE(shaft);
    // 165 mm inside the frame: 600 · ¼ + 20 + 40 · ¼ = 180 is too wide, 1:5 fits.
    CHECK(shaft->scale == Scale{1, 5});

    // Nothing fits a sheet taken up whole.
    CHECK_FALSE(PlaceStandardViews(RoomOf(sheet, {room.area}), bracket));
}

TEST_CASE("a free place: top right first, never over what is taken") {
    Drawing d;
    const SheetLayout sheet = LayoutSheet(d.sheets.front(), 1, 1);
    Room room = RoomOf(sheet);
    const auto spot = FreeSpot(room, {50, 40});
    REQUIRE(spot);
    CHECK(spot->x + 50 == doctest::Approx(room.area.max.x));
    CHECK(spot->y + 40 == doctest::Approx(room.area.max.y));
    Box2 taken;
    taken.Include(*spot);
    taken.Include(*spot + Vec2{50, 40});
    room.taken.push_back(taken);
    const auto next = FreeSpot(room, {50, 40});
    REQUIRE(next);
    Box2 b;
    b.Include(*next);
    b.Include(*next + Vec2{50, 40});
    CHECK(room.Fits(b));
    CHECK_FALSE(FreeSpot(room, {1000, 1000}));
}

#include "drawing/DrawingFile.h"

TEST_CASE("golden v1 with views: read, then written back the same") {
    const auto path = std::filesystem::path(BZ_SOURCE_DIR) / "tests" / "data" / "drawings" / "v1" / "a3-views.bzd";
    ReadReport report;
    const auto d = LoadDrawing(path, &report);
    REQUIRE(d);
    CHECK(report.warnings.empty());
    const std::vector<View> views = ViewsOf(d->sheets.front());
    REQUIRE(views.size() == 4);
    CHECK(views[0].direction == ViewDirection::Front);
    CHECK(views[1].direction == ViewDirection::Top);
    CHECK(views[1].parent == views[0].id);
    CHECK(views[2].direction == ViewDirection::Left);
    CHECK(views[2].at.y == doctest::Approx(views[0].at.y));
    CHECK(views[3].direction == ViewDirection::Iso);
    CHECK_FALSE(views[3].hidden);
    CHECK(d->models.front().path == "кронштейн.bzc");
    const auto again = FromJson(ToJson(*d));
    REQUIRE(again);
    CHECK(ToJson(*again) == ToJson(*d));
}
