// Positions of an assembly drawing (ГОСТ 2.109): the item in the file, the
// numbers the specification gives each component, and the leader on the sheet.
#include "drawing/Balloon.h"
#include "drawing/Drawing.h"
#include "drawing/SheetLayout.h"
#include "drawing/Specification.h"

#include <doctest.h>

using namespace bz;
using namespace bz::drawing;

TEST_CASE("position: written and read back; unknown keys stay; it is known") {
    Balloon b;
    b.id = ObjectId{12};
    b.view = ObjectId{7};
    b.component = ObjectId{31};
    b.point = {10, 0, 5};
    b.at = {80, 60};
    Item item = MakeBalloonItem(b);
    item.json["future"] = "x";
    const auto back = ReadBalloon(item);
    REQUIRE(back);
    CHECK(back->component == ObjectId{31});
    CHECK(back->point.z == doctest::Approx(5));
    CHECK(back->at == Vec2{80, 60});
    WriteBalloon(item, *back);
    CHECK(item.json["future"] == "x");
    CHECK(IsKnownItem(item));
    item.json.erase("component");
    CHECK_FALSE(ReadBalloon(item));
}

TEST_CASE("position: each component gets its row's number; equal ones share it") {
    SpecInput in;
    in.components = {{"вал.bzc", "БРЗ.001.002", "Вал", "", "", 1},
                     {"Стандартные изделия/Болт.bzc", "Болт М8×30 ГОСТ 7798-70", "Болт", "", "", 1},
                     {"корпус.bzc", "БРЗ.001.001", "Корпус", "", "", 1},
                     {"Стандартные изделия/Болт.bzc", "Болт М8×30 ГОСТ 7798-70", "Болт", "", "", 1}};
    const std::vector<std::string> p = SpecPositions(in);
    REQUIRE(p.size() == 4);
    CHECK(p[2] == "1");   // the parts by designation: корпус .001 first
    CHECK(p[0] == "2");
    CHECK(p[1] == "3");   // standard parts after the parts
    CHECK(p[3] == p[1]);
    // The rows say the same.
    for (const SpecRow& r : SpecRows(in))
        if (r.kind == SpecRow::Kind::Item && r.designation == "БРЗ.001.002") CHECK(r.position == "2");
}

TEST_CASE("position on the sheet: dot, leader, shelf away from the dot, the number over it; a lost one red") {
    View v;
    v.id = ObjectId{7};
    v.at = {100, 100};
    v.scale = {1, 2};
    Balloon b;
    b.id = ObjectId{12};
    b.view = v.id;
    b.at = {80, 40};   // on the sheet: 140, 120
    SheetLayout l;
    const Box2 box = AddBalloon(l, v, b, BalloonTarget{"3", {0, 0, 0}});   // the dot at the view's origin
    REQUIRE(l.labels.size() == 1);
    CHECK(l.labels[0].text == "3");
    CHECK(l.labels[0].height == doctest::Approx(kBalloonTextHeight));
    CHECK(l.labels[0].at.x > 140);   // the shelf runs right, away from the dot on its left
    CHECK(l.fills.size() == 1);
    CHECK(l.polylines.size() == 2);   // the shelf and the leader
    CHECK(box.min.x == doctest::Approx(100).epsilon(1e-3));

    SheetLayout lost;
    AddBalloon(lost, v, b, std::nullopt);
    REQUIRE(lost.labels.size() == 1);
    CHECK(lost.labels[0].text == "?");
    CHECK(lost.labels[0].problem);
    CHECK(lost.fills.empty());
}

TEST_CASE("positions arranged: a column beside the view, in the order of the dots, apart") {
    Box2 view;
    view.Include({100, 100});
    view.Include({200, 180});
    const std::vector<Vec2> dots{{150, 120}, {150, 175}, {160, 172}};
    const std::vector<Vec2> s = ArrangeShelves(view, dots, 420, 10);
    REQUIRE(s.size() == 3);
    for (Vec2 p : s) CHECK(p.x == doctest::Approx(215));   // right: more room there on an A3 sheet
    CHECK(s[1].y == doctest::Approx(175));                 // the highest at its dot's height
    CHECK(s[2].y == doctest::Approx(165));                 // the next under it, 10 mm apart
    CHECK(s[0].y == doctest::Approx(120));
    Box2 nearRight;
    nearRight.Include({300, 100});
    nearRight.Include({400, 150});
    CHECK(ArrangeShelves(nearRight, {{350, 120}}, 420)[0].x == doctest::Approx(285));   // left of it
}

TEST_CASE("positions laid out on the page with the content's numbers") {
    Drawing dr;
    View v;
    v.id = ObjectId{7};
    v.model = ObjectId{2};
    v.at = {100, 100};
    dr.sheets[0].views.push_back(MakeViewItem(v));
    Balloon b;
    b.id = ObjectId{12};
    b.view = v.id;
    b.component = ObjectId{31};
    b.at = {40, 40};
    dr.sheets[0].annotations.push_back(MakeBalloonItem(b));
    SheetContent content;
    std::string number = "4";
    content.balloon = [&](const Sheet&, const View&, const Balloon&) { return std::optional<BalloonTarget>{BalloonTarget{number, {}}}; };
    SheetLayout l = LayoutPage(dr, PagesOf(dr, content)[0], content);
    REQUIRE(l.hits.size() == 1);
    CHECK(l.labels.back().text == "4");
    number = "5";   // renumbered in the specification
    l = LayoutPage(dr, PagesOf(dr, content)[0], content);
    CHECK(l.labels.back().text == "5");
}
