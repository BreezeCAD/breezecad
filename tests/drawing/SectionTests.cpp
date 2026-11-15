// Sections of a drawing (ADR-0012, step 6): the view in the file, its cut
// made from a line on another view, hatching and the cutting line.
#include "drawing/Drawing.h"
#include "drawing/DrawingFile.h"
#include "drawing/DrawingView.h"
#include "drawing/SheetLayout.h"

#include <doctest.h>

#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

using namespace bz;
using namespace bz::drawing;

namespace {

View Front() {
    View v;
    v.id = ObjectId{3};
    v.model = ObjectId{2};
    v.direction = ViewDirection::Front;
    v.at = {100, 200};
    return v;
}

double Length(const std::vector<Polyline>& lines) {
    double sum = 0;
    for (const Polyline& p : lines) sum += (p.points.back() - p.points.front()).Length();
    return sum;
}

} // namespace

TEST_CASE("section: a horizontal line on the front view, the section put under it, looks from above") {
    const View front = Front();
    const SectionCut cut = MakeCut(front, {-70, 15}, {70, 15}, {0, -60}, "А");
    CHECK(cut.of == front.id);
    CHECK(cut.eye.z == doctest::Approx(1));
    CHECK(cut.up.y == doctest::Approx(1));     // as the view from above (first angle)
    CHECK(cut.point.z == doctest::Approx(15));
    // A vertical line, the section to the right: looks from the left, up stays up.
    const SectionCut side = MakeCut(front, {30, -10}, {30, 50}, {80, 0}, "Б");
    CHECK(side.eye.x == doctest::Approx(-1));
    CHECK(side.up.z == doctest::Approx(1));
    CHECK(side.point.x == doctest::Approx(30));
}

TEST_CASE("section: written and read back; an older program keeps it as it is") {
    View s;
    s.id = ObjectId{8};
    s.model = ObjectId{2};
    s.direction = ViewDirection::Section;
    s.hidden = false;
    s.section = MakeCut(Front(), {-70, 15}, {70, 15}, {0, -60}, "В");
    Item item = MakeViewItem(s);
    item.json["section"]["future"] = true;
    const auto back = ReadView(item);
    REQUIRE(back);
    REQUIRE(back->section);
    CHECK(back->section->letter == "В");
    CHECK(back->section->of == ObjectId{3});
    CHECK(back->section->b == Vec2{70, 15});
    CHECK(ProjectPoint(*back, {10, 20, 15}).y == doctest::Approx(20));   // seen from above
    WriteView(item, *back);
    CHECK(item.json["section"]["future"] == true);
    CHECK(item.json["direction"] == "section");   // a direction the earlier versions do not know: they keep the view

    Item broken = item;
    broken.json["section"].erase("eye");
    CHECK_FALSE(ReadView(broken));   // kept as it is, not shown

    CHECK(NextSectionLetter(Sheet{}) == "А");
    Sheet sheet;
    sheet.views.push_back(item);
    s.section->letter = "А";
    sheet.views.push_back(MakeViewItem(s));
    CHECK(NextSectionLetter(sheet) == "Б");
}

TEST_CASE("hatching: 45° thin lines inside, the hole left clear") {
    const std::vector<Vec2> square{{0, 0}, {20, 0}, {20, 20}, {0, 20}};
    const std::vector<Polyline> full = Hatch({square}, 2);
    REQUIRE_FALSE(full.empty());
    for (const Polyline& p : full) {
        CHECK(p.weight == LineWeight::Thin);
        const Vec2 d = p.points.back() - p.points.front();
        CHECK(std::abs(std::abs(d.x) - std::abs(d.y)) < 1e-9);   // at 45°
        for (Vec2 q : p.points) {
            CHECK(q.x >= -1e-9);
            CHECK(q.x <= 20 + 1e-9);
        }
    }
    // The hatched area is the square's: the length of all lines times the step.
    CHECK(Length(full) * 2 == doctest::Approx(400).epsilon(0.05));
    const std::vector<Vec2> hole{{5, 5}, {15, 5}, {15, 15}, {5, 15}};
    CHECK(Length(Hatch({square, hole}, 2)) * 2 == doctest::Approx(300).epsilon(0.08));
}

TEST_CASE("section on the sheet: cut faces hatched, «А–А» over it, the cutting line on the front view") {
    Drawing dr;
    Sheet& sheet = dr.sheets[0];
    const View front = Front();
    sheet.views.push_back(MakeViewItem(front));
    View s;
    s.id = ObjectId{8};
    s.model = front.model;
    s.direction = ViewDirection::Section;
    s.at = {100, 100};
    s.section = MakeCut(front, {-70, 15}, {70, 15}, {0, -60}, "А");
    sheet.views.push_back(MakeViewItem(s));
    ViewLines frontLines, cutLines;
    frontLines.visible = {{{-60, 0}, {60, 0}, {60, 30}, {-60, 30}, {-60, 0}}};
    cutLines.visible = {{{-60, -40}, {60, -40}, {60, 40}, {-60, 40}, {-60, -40}}};
    cutLines.sections = {{{{-60, -40}, {60, -40}, {60, 40}, {-60, 40}}}};
    SheetContent content;
    content.lines = [&](const Sheet&, const View& v) { return v.id == front.id ? &frontLines : &cutLines; };
    const SheetLayout l = LayoutPage(dr, PagesOf(dr, content)[0], content);
    std::size_t hatch = 0;
    for (const Polyline& p : l.polylines)
        if (p.weight == LineWeight::Thin && p.points.size() == 2 && p.points[0].y < 150) ++hatch;
    CHECK(hatch > 20);
    bool label = false;
    int letters = 0;
    for (const Label& lb : l.labels) {
        if (lb.text == "А–А") {
            label = true;
            CHECK(lb.at.y > 140);   // over the section's box (its top at 100 + 40)
        }
        if (lb.text == "А") ++letters;
    }
    CHECK(label);
    CHECK(letters == 2);           // at both ends of the cutting line
    CHECK(l.fills.size() == 2);    // its two arrows
    // The thick strokes at the ends of the line, on the front view at y = 200 + 15.
    int strokes = 0;
    for (const Stroke& st : l.strokes)
        if (st.weight == LineWeight::Thick && std::abs(st.a.y - 215) < 1e-9 && std::abs(st.b.y - 215) < 1e-9) ++strokes;
    CHECK(strokes == 2);
}

TEST_CASE("golden v1 with sections: read, then written back the same") {
    const auto path = std::filesystem::path(BZ_SOURCE_DIR) / "tests" / "data" / "drawings" / "v1" / "a3-sections.bzd";
    ReadReport report;
    const auto d = LoadDrawing(path, &report);
    REQUIRE(d);
    CHECK(report.warnings.empty());
    std::vector<std::string> letters;
    for (const View& v : ViewsOf(d->sheets.front()))
        if (v.section) letters.push_back(v.section->letter);
    CHECK(letters == std::vector<std::string>{"А", "Б"});
    const auto again = FromJson(ToJson(*d));
    REQUIRE(again);
    CHECK(ToJson(*again) == ToJson(*d));
}
