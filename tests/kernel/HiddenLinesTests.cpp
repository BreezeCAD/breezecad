// Lines of drawing views (ADR-0012, step 3): a plate with a hole through it
// seen from the front and from above — where the lines are, which are hidden,
// and the centre lines of the hole.
#include "../Fixtures.h"
#include "document/HoleFeature.h"
#include "document/BzcFile.h"
#include "document/JsonIO.h"
#include "drawing/Dimension.h"
#include "drawing/DrawingFile.h"
#include "drawing/DrawingView.h"
#include "kernel/Model.h"
#include "kernel/drawing/HiddenLines.h"

#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>

// The fast set: every pull request runs it (bz_tests -ts=fast, STAB-0 step 4).
TEST_SUITE_BEGIN("fast");

using namespace bz;
using namespace bz::test;

namespace {

/// 120 × 80 × 30 on the XY plane (z 0…30), a Ø10 hole through it at the centre.
kernel::Model Plate() {
    doc::Document d;
    auto& sk = AddSketch(d);
    AddRect(d, sk, {-60, -40}, {60, 40});
    AddExtrude(d, sk.id, "30");
    auto h = std::make_unique<doc::HoleFeature>();
    h->id = d.Ids().Next();
    h->name = d.UniqueFeatureName("Отверстие");
    h->face = {{0, 0, 1}, {10, 5, 30}};
    h->centres = {{0, 0}};
    h->type = doc::HoleType::Simple;
    h->throughAll = true;
    h->diameter.expression = "10";
    d.Insert(std::move(h), d.FeatureCount());
    kernel::Model model;
    model.Rebuild(d);
    return model;
}

Box2 BoxOf(const std::vector<std::vector<Vec2>>& lines) {
    Box2 box;
    for (const auto& l : lines)
        for (Vec2 p : l) box.Include(p);
    return box;
}

kernel::ProjectedLines Look(const kernel::ViewShapes& shapes, drawing::ViewDirection direction) {
    const drawing::ViewBasis b = drawing::BasisOf(direction);
    auto lines = shapes.Project(b.eye, b.up);
    REQUIRE(lines);
    return *lines;
}

} // namespace

TEST_CASE("view lines: the front of a plate with a hole — the hole hidden, its axis") {
    const kernel::Model model = Plate();
    const kernel::ViewShapes shapes = kernel::ViewShapes::Of(model);
    REQUIRE_FALSE(shapes.Empty());
    const kernel::ProjectedLines front = Look(shapes, drawing::ViewDirection::Front);

    // The outline: x as the model's x, y as its z.
    const Box2 visible = BoxOf(front.visible);
    CHECK(visible.min.x == doctest::Approx(-60).epsilon(1e-6));
    CHECK(visible.max.x == doctest::Approx(60).epsilon(1e-6));
    CHECK(visible.min.y == doctest::Approx(0).epsilon(1e-6));
    CHECK(visible.max.y == doctest::Approx(30).epsilon(1e-6));
    // The hole's walls behind the front face: hidden, at x = ±5.
    const Box2 hidden = BoxOf(front.hidden);
    REQUIRE_FALSE(hidden.IsEmpty());
    CHECK(hidden.min.x == doctest::Approx(-5).epsilon(1e-6));
    CHECK(hidden.max.x == doctest::Approx(5).epsilon(1e-6));
    // Seen from the side: an axis, vertical, through the plate.
    REQUIRE(front.axes.size() == 1);
    CHECK(front.axes[0].a.x == doctest::Approx(0).scale(1));
    CHECK(std::abs(front.axes[0].b.y - front.axes[0].a.y) == doctest::Approx(30));
    CHECK(front.marks.empty());
}

TEST_CASE("view lines: from above — the hole a circle with a centre mark; left view mirrors y") {
    const kernel::Model model = Plate();
    const kernel::ViewShapes shapes = kernel::ViewShapes::Of(model);
    const kernel::ProjectedLines top = Look(shapes, drawing::ViewDirection::Top);
    const Box2 visible = BoxOf(top.visible);
    CHECK(visible.min.y == doctest::Approx(-40).epsilon(1e-6));
    CHECK(visible.max.y == doctest::Approx(40).epsilon(1e-6));
    CHECK(top.hidden.empty());   // a through hole hides nothing from above
    REQUIRE(top.marks.size() == 1);
    CHECK(top.marks[0].radius == doctest::Approx(5));
    CHECK(top.axes.empty());

    // Left of the front view's viewer: x is minus the model's y, as drawing::ProjectPoint says.
    const kernel::ProjectedLines left = Look(shapes, drawing::ViewDirection::Left);
    const Box2 l = BoxOf(left.visible);
    CHECK(l.min.x == doctest::Approx(-40).epsilon(1e-6));
    CHECK(l.max.y == doctest::Approx(30).epsilon(1e-6));
    CHECK(drawing::ProjectPoint(drawing::ViewDirection::Left, {0, 40, 30}).x == doctest::Approx(-40));
    CHECK(drawing::ProjectPoint(drawing::ViewDirection::Front, {60, 0, 30}).x == doctest::Approx(60));
    CHECK(drawing::ProjectPoint(drawing::ViewDirection::Top, {0, 40, 0}).y == doctest::Approx(40));
}

TEST_CASE("view lines: an isometry has lines; an empty model none") {
    const kernel::Model model = Plate();
    const kernel::ProjectedLines iso = Look(kernel::ViewShapes::Of(model), drawing::ViewDirection::Iso);
    CHECK(iso.visible.size() > 10);
    CHECK(iso.marks.empty());   // slanted: no centre lines
    kernel::Model empty;
    CHECK(kernel::ViewShapes::Of(empty).Empty());
    CHECK_FALSE(kernel::ViewShapes().Project({0, -1, 0}, {0, 0, 1}));
}

TEST_CASE("dimension edges: picked on a view, found again after the plate grows") {
    const kernel::ViewShapes shapes = kernel::ViewShapes::Of(Plate());
    const drawing::ViewBasis front = drawing::BasisOf(drawing::ViewDirection::Front);
    // The top edge of the front view: y = 30 there, from x −60 to 60 (the front one, nearer the viewer).
    const auto top = shapes.EdgeAt(front.eye, front.up, {10, 30.2}, 1.0);
    REQUIRE(top);
    CHECK(top->edge.kind == kernel::ModelEdge::Kind::Line);
    CHECK(std::abs(top->edge.a.x - top->edge.b.x) == doctest::Approx(120));
    CHECK(top->edge.a.y == doctest::Approx(-40));   // the near side: the front view looks along +Y
    CHECK_FALSE(shapes.EdgeAt(front.eye, front.up, {10, 15}, 1.0));   // the middle of the face: no edge

    // The hole's rim from above.
    const drawing::ViewBasis above = drawing::BasisOf(drawing::ViewDirection::Top);
    const auto rim = shapes.EdgeAt(above.eye, above.up, {5.1, 0}, 0.5);
    REQUIRE(rim);
    CHECK(rim->edge.kind == kernel::ModelEdge::Kind::Circle);
    CHECK(rim->edge.radius == doctest::Approx(5));
    CHECK(rim->edge.center.z == doctest::Approx(30));   // the upper rim, nearer the viewer

    // Found again on the same bodies.
    const auto again = shapes.Resolve({top->ref, rim->ref});
    REQUIRE(again.size() == 2);
    REQUIRE(again[0]);
    REQUIRE(again[1]);
    CHECK(again[1]->radius == doctest::Approx(5));
}

TEST_CASE("dimension edges: the golden drawing's dimensions on the bracket they were made on") {
    // a3-dimensions.bzd was made in the program on кронштейн.bzc: a linear one,
    // a diameter and an angle (ADR-0012, step 5). Its references must keep finding the edges.
    const std::filesystem::path data = std::filesystem::path(BZ_SOURCE_DIR) / "tests" / "data";
    auto part = doc::LoadBzc(data / "кронштейн.bzc");
    REQUIRE(part);
    kernel::Model model;
    model.Rebuild(*part);
    const kernel::ViewShapes shapes = kernel::ViewShapes::Of(model);
    const auto drawing = drawing::LoadDrawing(data / "drawings" / "v1" / "a3-dimensions.bzd");
    REQUIRE(drawing);
    const drawing::Sheet& sheet = drawing->sheets.front();
    const std::vector<drawing::View> views = drawing::ViewsOf(sheet);
    const std::vector<drawing::Dimension> dims = drawing::DimensionsOf(sheet);
    REQUIRE(dims.size() == 3);
    std::vector<double> values;
    for (const drawing::Dimension& dim : dims) {
        std::vector<doc::EdgeReference> refs(dim.edges.size());
        for (std::size_t i = 0; i < refs.size(); ++i) REQUIRE(doc::json_io::Read(dim.edges[i], refs[i]));
        const auto found = shapes.Resolve(refs);
        std::vector<std::optional<drawing::ModelEdge>> edges;
        for (const auto& e : found) {
            REQUIRE(e);
            drawing::ModelEdge x;
            x.kind = e->kind == kernel::ModelEdge::Kind::Line ? drawing::ModelEdge::Kind::Line
                     : e->kind == kernel::ModelEdge::Kind::Circle ? drawing::ModelEdge::Kind::Circle
                                                                  : drawing::ModelEdge::Kind::Other;
            x.a = e->a;
            x.b = e->b;
            x.center = e->center;
            x.axis = e->axis;
            x.radius = e->radius;
            edges.push_back(x);
        }
        const auto view = std::find_if(views.begin(), views.end(), [&](const drawing::View& v) { return v.id == dim.view; });
        REQUIRE(view != views.end());
        const auto m = drawing::MeasureOf(*view, dim, edges);
        REQUIRE(m);
        values.push_back(m->value);
    }
    CHECK(values[0] == doctest::Approx(110));   // the plate's bottom edge, between its fillets
    CHECK(values[1] == doctest::Approx(8));     // a fixing hole
    CHECK(values[2] == doctest::Approx(90));
}

TEST_CASE("section: the plate cut at half its height — the cut face with the hole in it") {
    const kernel::ViewShapes shapes = kernel::ViewShapes::Of(Plate());
    // As MakeCut makes it for a horizontal line through the front view, the section put under it.
    const auto lines = shapes.Project({0, 0, 1}, {0, 1, 0}, kernel::CutPlane{{0, 0, 15}, {0, 0, 1}});
    REQUIRE(lines);
    REQUIRE(lines->sections.size() == 1);
    REQUIRE(lines->sections[0].size() == 2);   // the outline and the hole
    Box2 outer, inner;
    for (Vec2 p : lines->sections[0][0]) outer.Include(p);
    for (Vec2 p : lines->sections[0][1]) inner.Include(p);
    if (outer.max.x - outer.min.x < inner.max.x - inner.min.x) std::swap(outer, inner);
    CHECK(outer.max.x - outer.min.x == doctest::Approx(120).epsilon(1e-6));
    CHECK(outer.max.y - outer.min.y == doctest::Approx(80).epsilon(1e-6));
    CHECK(inner.max.x - inner.min.x == doctest::Approx(10).epsilon(1e-3));
    // The upper half is gone: nothing of z above 15 is left to see.
    const Box2 seen = BoxOf(lines->visible);
    CHECK(seen.max.x == doctest::Approx(60).epsilon(1e-6));
    // A plane that misses the part: no cut faces.
    const auto off = shapes.Project({0, 0, 1}, {0, 1, 0}, kernel::CutPlane{{0, 0, 50}, {0, 0, 1}});
    REQUIRE(off);
    CHECK(off->sections.empty());
}

TEST_CASE("section: the golden drawing's sections of the bracket have cut faces") {
    const std::filesystem::path data = std::filesystem::path(BZ_SOURCE_DIR) / "tests" / "data";
    auto part = doc::LoadBzc(data / "кронштейн.bzc");
    REQUIRE(part);
    kernel::Model model;
    model.Rebuild(*part);
    const kernel::ViewShapes shapes = kernel::ViewShapes::Of(model);
    const auto drawing = drawing::LoadDrawing(data / "drawings" / "v1" / "a3-sections.bzd");
    REQUIRE(drawing);
    int sections = 0;
    for (const drawing::View& v : drawing::ViewsOf(drawing->sheets.front())) {
        if (!v.section) continue;
        ++sections;
        const drawing::ViewBasis b = drawing::BasisOf(v);
        const auto lines = shapes.Project(b.eye, b.up, kernel::CutPlane{v.section->point, v.section->eye});
        REQUIRE(lines);
        CHECK_FALSE(lines->sections.empty());
        CHECK_FALSE(lines->visible.empty());
    }
    CHECK(sections == 2);
}

TEST_SUITE_END();
