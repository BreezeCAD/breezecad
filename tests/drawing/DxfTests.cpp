// The drawing as DXF (ADR-0012): the sheet's lines and texts on layers by
// ГОСТ 2.303, dashes as pieces, sheets side by side — read back by our own
// DXF reader as other programs would read it.
#include "drawing/Dimension.h"
#include "drawing/DrawingDxf.h"
#include "drawing/DrawingView.h"
#include "sheetmetal/DxfDrawing.h"

#include <doctest.h>

#include <filesystem>
#include <fstream>

using namespace bz;
using namespace bz::drawing;

TEST_CASE("DXF: the frame and title block of an A3 sheet, its texts, read back") {
    Drawing d;
    d.sheets.front().title.fields[field::kName] = "Вал";
    const std::string text = DrawingDxf(d);
    CHECK(text.find("AC1009") != std::string::npos);
    const auto back = dxf::Read(text);
    REQUIRE(back);
    const Box2 all = dxf::Bounds(*back);
    CHECK(all.max.x - all.min.x == doctest::Approx(420).epsilon(0.01));   // A3 landscape
    CHECK(all.max.y - all.min.y == doctest::Approx(297).epsilon(0.01));
    bool name = false;
    for (const dxf::Entity& e : back->entities)
        if (e.kind == dxf::Kind::Text && e.text == "Вал") {
            name = true;
            CHECK(e.layer == "TEXT");
        }
    CHECK(name);
    CHECK(back->FindLayer("THICK"));
    CHECK(back->FindLayer("HIDDEN"));
}

TEST_CASE("DXF: a view's hidden line in dashes, a dimension with its arrows; two sheets side by side") {
    Drawing d;
    View v;
    v.id = ObjectId{7};
    v.model = ObjectId{2};
    v.at = {100, 100};
    d.sheets.front().views.push_back(MakeViewItem(v));
    Dimension dim;
    dim.id = ObjectId{9};
    dim.view = v.id;
    dim.axis = DimensionAxis::Horizontal;
    dim.edges = {Json{{"middle", Json::array({0, 0, 0})}}};
    dim.at = {0, -20};
    d.sheets.front().annotations.push_back(MakeDimensionItem(dim));
    Sheet second;
    second.id = d.Ids().Next();
    second.size = SheetSize::A4;
    d.sheets.push_back(second);
    ViewLines lines;
    lines.visible = {{{-50, 0}, {50, 0}}};
    lines.hidden = {{{-50, 10}, {50, 10}}};   // 100 mm dashed: strokes of 4, gaps of 1.5
    SheetContent content;
    content.lines = [&](const Sheet&, const View&) { return &lines; };
    content.edges = [](const Sheet&, const View&, const std::vector<Json>& refs) {
        ModelEdge e;
        e.kind = ModelEdge::Kind::Line;
        e.a = {-50, 0, 0};
        e.b = {50, 0, 0};
        return std::vector<std::optional<ModelEdge>>(refs.size(), e);
    };
    const auto back = dxf::Read(DrawingDxf(d, content));
    REQUIRE(back);
    int dashes = 0, arrows = 0;
    bool value = false;
    for (const dxf::Entity& e : back->entities) {
        if (e.layer == "HIDDEN") ++dashes;
        if (e.kind == dxf::Kind::Polyline && e.closed && e.points.size() == 3) ++arrows;
        if (e.kind == dxf::Kind::Text && e.text == "100") value = true;
    }
    CHECK(dashes == 19);   // 100 / 5.5 → 18 whole strokes and a piece
    CHECK(arrows == 2);
    CHECK(value);
    // The A4 sheet after the A3 one and the gap.
    const Box2 all = dxf::Bounds(*back);
    CHECK(all.max.x == doctest::Approx(420 + kDxfSheetGap + 210).epsilon(0.001));
}

TEST_CASE("DXF: written through a temporary file; a bad folder is an error") {
    const auto dir = std::filesystem::temp_directory_path() / "bz-drawing-dxf";
    std::filesystem::create_directories(dir);
    const auto file = dir / "Чертёж.dxf";
    REQUIRE(ExportDrawingDxf(Drawing{}, file));
    CHECK(std::filesystem::file_size(file) > 1000);
    CHECK_FALSE(std::filesystem::exists(dir / "Чертёж.dxf.part"));
    CHECK_FALSE(ExportDrawingDxf(Drawing{}, dir / "нет такой папки" / "x.dxf"));
    std::filesystem::remove_all(dir);
}
