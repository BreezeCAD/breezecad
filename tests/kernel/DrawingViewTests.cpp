// Views of a part for a drawing (ADR-0012, step 3): hidden line removal on
// three parts whose lines can be counted by hand — a block with a hole, a
// stepped shaft with a chamfer, a block with a fillet and a blind hole.
// Lengths are checked, not edge counts: how OpenCASCADE splits an edge is
// its own business. With BZ_SAMPLE_OUT set, each part's three views are also
// written there as a PDF sheet (to look at).

#include "../Fixtures.h"
#include "document/EdgeFeature.h"
#include "drawing/DrawingPdf.h"
#include "drawing/ViewLayout.h"
#include "kernel/Model.h"
#include "kernel/drawing/ViewProjection.h"
#include "kernel/occt/EdgeMatch.h"
#include "kernel/occt/ModelImpl.h"

#include <BRepAdaptor_Curve.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <doctest.h>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

using namespace bz;
using namespace bz::test;
using doc::FeatureState;
using drawing::ViewLines;
using drawing::ViewProjection;

namespace {

constexpr double kTol = 0.05;   // mm: polylines are a little shorter than the curves they follow

template <class T>
T& AddFeature(doc::Document& d, const std::string& name) {
    auto f = std::make_unique<T>();
    f->id = d.Ids().Next();
    f->name = d.UniqueFeatureName(name);
    auto* raw = f.get();
    d.Insert(std::move(f), d.FeatureCount());
    return *raw;
}

/// References to the edges of the last body that `keep` accepts.
std::vector<doc::EdgeReference> Edges(const kernel::Model& model, const std::function<bool(const BRepAdaptor_Curve&)>& keep) {
    const TopoDS_Shape& body = model.GetImpl().bodies.back().shape;
    std::vector<doc::EdgeReference> refs;
    TopTools_IndexedMapOfShape edges;   // each edge once (an explorer meets it on both faces)
    TopExp::MapShapes(body, TopAbs_EDGE, edges);
    for (int i = 1; i <= edges.Extent(); ++i) {
        const TopoDS_Edge& edge = TopoDS::Edge(edges(i));
        if (keep(BRepAdaptor_Curve(edge))) refs.push_back(kernel::occt::DescribeEdge(edge, body));
    }
    return refs;
}

ViewLines View(const kernel::Model& model, ViewProjection projection) {
    Expected<ViewLines> lines = kernel::ProjectView(model, kernel::AxesOf(projection));
    REQUIRE(lines);
    return *lines;
}

/// `expected` millimetres, give or take `tolerance`.
doctest::Approx Mm(double expected, double tolerance = kTol) { return doctest::Approx(expected).epsilon(tolerance / expected); }

/// A block 120 x 80 x 30 (x -60…60, y -40…40, z 0…30).
doc::Document Block() {
    doc::Document d;
    auto& sketch = AddSketch(d);
    AddRect(d, sketch, {-60, -40}, {60, 40});
    AddExtrude(d, sketch.id, "30");
    return d;
}

/// A round pocket of diameter 20 in the middle, from the bottom (z = 0) up by `depth`.
void AddPocket(doc::Document& d, const std::string& depth) {
    auto& sketch = AddSketch(d);
    AddCircle(d, sketch, {0, 0}, 10);
    AddExtrude(d, sketch.id, depth, doc::BooleanOp::Cut);
}

/// With BZ_SAMPLE_OUT set: the three standard views of the model on an A3 sheet, as `name`.pdf there.
void WriteSample(const kernel::Model& model, const char* name) {
    const char* out = std::getenv("BZ_SAMPLE_OUT");
    if (!out) return;
    namespace dw = bz::drawing;
    dw::Drawing d;
    d.sheets[0].title.fields[dw::field::kName] = name;
    const auto front = std::make_shared<const ViewLines>(View(model, ViewProjection::Front));
    const auto top = std::make_shared<const ViewLines>(View(model, ViewProjection::Top));
    const auto left = std::make_shared<const ViewLines>(View(model, ViewProjection::Left));
    const auto plan = dw::PlanStandardViews(dw::LayoutSheet(d.sheets[0], 1, 1), *front, *top, *left);
    REQUIRE(plan);
    d.sheets[0].title.fields[dw::field::kScale] = dw::ScaleText(plan->scale);
    const ObjectId model0{1}, a = d.Ids().Next(), b = d.Ids().Next(), c = d.Ids().Next();
    d.sheets[0].views.push_back(dw::MakeViewItem({a, model0, ViewProjection::Front, plan->scale, plan->front, {}, true}));
    d.sheets[0].views.push_back(dw::MakeViewItem({b, model0, ViewProjection::Top, std::nullopt, plan->top, a, true}));
    d.sheets[0].views.push_back(dw::MakeViewItem({c, model0, ViewProjection::Left, std::nullopt, plan->left, a, true}));
    const dw::ViewImages images{{a, front}, {b, top}, {c, left}};
    const auto font = dw::TrueTypeFont::FromFile(std::filesystem::path(BZ_SOURCE_DIR) / "assets" / "fonts" / "IBMPlexSans-Regular.ttf");
    REQUIRE(font);
    REQUIRE(dw::ExportDrawingPdf(d, std::filesystem::path(out) / (std::string(name) + ".pdf"), *font, "BreezeCAD test", &images));
}

bool AllOk(const doc::Document& d) {
    bool ok = true;
    for (const auto& f : d.Features())
        if (f->status.state != FeatureState::Ok) {
            MESSAGE(f->name << ": " << f->status.message);
            ok = false;
        }
    return ok;
}

} // namespace

TEST_CASE("drawing views: the standard axes follow GOST 2.305, first angle") {
    // The main view is the program's «Вид спереди»: x to the right, z up, looking along +Y.
    const kernel::ViewAxes front = kernel::AxesOf(ViewProjection::Front);
    CHECK(front.right.Cross(front.up) == Vec3{0, -1, 0});
    // From above: looking down, and the model's back (+Y) is toward the main view above it.
    const kernel::ViewAxes top = kernel::AxesOf(ViewProjection::Top);
    CHECK(top.right == front.right);
    CHECK(top.up == Vec3{0, 1, 0});
    CHECK(top.right.Cross(top.up) == Vec3{0, 0, 1});
    // From the left: looking along +X; the view stands to the right of the main one with the
    // model's back (+Y) toward it, so its «right» is -Y. Heights match the main view.
    const kernel::ViewAxes left = kernel::AxesOf(ViewProjection::Left);
    CHECK(left.up == front.up);
    CHECK(left.right == Vec3{0, -1, 0});
    CHECK(left.right.Cross(left.up) == Vec3{-1, 0, 0});
}

TEST_CASE("drawing views: a block with a through hole") {
    doc::Document d = Block();
    AddPocket(d, "30");
    kernel::Model model;
    model.Rebuild(d);
    REQUIRE(AllOk(d));
    WriteSample(model, "views-block-hole");

    SUBCASE("the main view: the outline is seen, the hole is two dashed lines") {
        const ViewLines v = View(model, ViewProjection::Front);
        CHECK(drawing::TotalLength(v.visible) == Mm(2 * (120 + 30)));
        CHECK(drawing::TotalLength(v.hidden) == Mm(2 * 30));
        CHECK(drawing::Covers(v.hidden, {-10, 0}, {-10, 30}, kTol));
        CHECK(drawing::Covers(v.hidden, {10, 0}, {10, 30}, kTol));
        CHECK(drawing::Covers(v.visible, {-60, 30}, {60, 30}, kTol));
        CHECK(v.min.x == doctest::Approx(-60));
        CHECK(v.max.x == doctest::Approx(60));
        CHECK(v.min.y == doctest::Approx(0));
        CHECK(v.max.y == doctest::Approx(30));
    }
    SUBCASE("from above: the outline and the circle, nothing hidden") {
        const ViewLines v = View(model, ViewProjection::Top);
        CHECK(drawing::TotalLength(v.visible) == Mm(2 * (120 + 80) + 2 * kPi * 10));
        // The bottom edges lie under the top ones: drawn once, as seen.
        CHECK(drawing::TotalLength(v.hidden) < kTol);
        CHECK(v.max.y == doctest::Approx(40));
    }
    SUBCASE("from the left: as the main view, 80 wide") {
        const ViewLines v = View(model, ViewProjection::Left);
        CHECK(drawing::TotalLength(v.visible) == Mm(2 * (80 + 30)));
        CHECK(drawing::TotalLength(v.hidden) == Mm(2 * 30));
        CHECK(v.min.x == doctest::Approx(-40));
        CHECK(v.max.x == doctest::Approx(40));
    }
}

TEST_CASE("drawing views: a stepped shaft with a chamfer") {
    // Along X: diameter 40 from 0 to 60, diameter 24 from 60 to 100, a 2 x 45° chamfer at the end.
    doc::Document d;
    auto& sketch = AddSketch(d);
    auto& s = sketch.sketch;
    const std::vector<Vec2> profile{{0, 0}, {100, 0}, {100, 12}, {60, 12}, {60, 20}, {0, 20}};
    std::vector<ObjectId> points;
    for (const Vec2 p : profile) points.push_back(s.AddPoint(d.Ids(), p));
    for (std::size_t i = 0; i < points.size(); ++i) s.AddLine(d.Ids(), points[i], points[(i + 1) % points.size()]);
    AddRevolve(d, sketch.id, {doc::RevolveAxisKind::SketchX, {}});
    kernel::Model model;
    model.Rebuild(d);
    REQUIRE(AllOk(d));
    auto& chamfer = AddFeature<doc::ChamferFeature>(d, "Фаска");
    chamfer.distance.expression = "2";
    chamfer.edges = Edges(model, [](const BRepAdaptor_Curve& c) {
        return c.GetType() == GeomAbs_Circle && std::abs(c.Circle().Radius() - 12) < 1e-6 && std::abs(c.Circle().Location().X() - 100) < 1e-6;
    });
    REQUIRE(chamfer.edges.size() == 1);
    model.Rebuild(d);
    REQUIRE(AllOk(d));

    WriteSample(model, "views-shaft");

    const double slant = std::sqrt(8.0);   // the chamfer's side as seen
    const double outline = 2 * 60 + 40 + 40 + 2 * 38 + 2 * slant + 24 + 20;

    SUBCASE("the main view: silhouettes of the steps, the shoulder and the chamfer; nothing hidden") {
        const ViewLines v = View(model, ViewProjection::Front);
        CHECK(drawing::TotalLength(v.visible) == Mm(outline, 0.1));
        CHECK(drawing::TotalLength(v.hidden) < kTol);
        // The silhouette of each cylinder — there is no edge of the model there.
        CHECK(drawing::Covers(v.visible, {0, 20}, {60, 20}, kTol));
        CHECK(drawing::Covers(v.visible, {60, -12}, {98, -12}, kTol));
        // The chamfer: its silhouette and the circle where it starts, seen from its side.
        CHECK(drawing::Covers(v.visible, {98, 12}, {100, 10}, kTol));
        CHECK(drawing::Covers(v.visible, {98, -12}, {98, 12}, kTol));
        // No seam of a cylinder along the axis.
        CHECK_FALSE(drawing::Covers(v.visible, {10, 0}, {50, 0}, kTol));
    }
    SUBCASE("from above it is the same picture: a body of revolution") {
        const ViewLines v = View(model, ViewProjection::Top);
        CHECK(drawing::TotalLength(v.visible) == Mm(outline, 0.1));
        CHECK(drawing::TotalLength(v.hidden) < kTol);
    }
    SUBCASE("from the left: the large end is seen, the thinner step and the chamfer are behind it") {
        const ViewLines v = View(model, ViewProjection::Left);
        CHECK(drawing::TotalLength(v.visible) == Mm(2 * kPi * 20, 0.1));
        // Diameter 24 (the step; the chamfer starts on the same circle) and diameter 20 (the end face).
        CHECK(drawing::TotalLength(v.hidden) == Mm(2 * kPi * 12 + 2 * kPi * 10, 0.1));
        CHECK(v.Width() == doctest::Approx(40));
        CHECK(v.Height() == doctest::Approx(40));
    }
}

TEST_CASE("drawing views: a fillet and a blind hole") {
    // The block with its top front edge (y = -40, z = 30) rounded by 5 and a pocket
    // of diameter 20 from the bottom, 15 deep: nothing of the pocket is seen from above.
    doc::Document d = Block();
    AddPocket(d, "15");
    kernel::Model model;
    model.Rebuild(d);
    REQUIRE(AllOk(d));
    auto& fillet = AddFeature<doc::FilletFeature>(d, "Сопряжение");
    fillet.radius.expression = "5";
    fillet.edges = Edges(model, [](const BRepAdaptor_Curve& c) {
        if (c.GetType() != GeomAbs_Line) return false;
        const gp_Pnt mid = c.Value((c.FirstParameter() + c.LastParameter()) / 2);
        return std::abs(mid.Y() + 40) < 1e-6 && std::abs(mid.Z() - 30) < 1e-6;
    });
    REQUIRE(fillet.edges.size() == 1);
    model.Rebuild(d);
    REQUIRE(AllOk(d));

    WriteSample(model, "views-fillet-blind-hole");

    SUBCASE("the main view: the fillet's tangent edge is not drawn; the pocket is dashed") {
        const ViewLines v = View(model, ViewProjection::Front);
        CHECK(drawing::TotalLength(v.visible) == Mm(2 * (120 + 30), 0.1));
        // Where the fillet meets the front face (z = 25): a smooth edge, no line.
        CHECK_FALSE(drawing::Covers(v.visible, {-50, 25}, {50, 25}, kTol));
        CHECK_FALSE(drawing::Covers(v.hidden, {-50, 25}, {50, 25}, kTol));
        // The pocket: its two sides and its bottom.
        CHECK(drawing::TotalLength(v.hidden) == Mm(15 + 15 + 20, 0.1));
        CHECK(drawing::Covers(v.hidden, {-10, 15}, {10, 15}, kTol));
        CHECK(drawing::Covers(v.hidden, {10, 0}, {10, 15}, kTol));
    }
    SUBCASE("from above: the pocket is a dashed circle; no line along the fillet") {
        const ViewLines v = View(model, ViewProjection::Top);
        CHECK(drawing::TotalLength(v.visible) == Mm(2 * (120 + 80), 0.1));
        CHECK(drawing::TotalLength(v.hidden) == Mm(2 * kPi * 10, 0.1));
        CHECK_FALSE(drawing::Covers(v.visible, {-50, -35}, {50, -35}, kTol));
    }
    SUBCASE("from the left: the fillet is the rounded corner of the outline") {
        const ViewLines v = View(model, ViewProjection::Left);
        // The rectangle 80 x 30 with one corner rounded by 5: two legs of 5 become a quarter circle.
        CHECK(drawing::TotalLength(v.visible) == Mm(2 * (80 + 30) - 2 * 5 + kPi * 5 / 2, 0.1));
        CHECK(drawing::TotalLength(v.hidden) == Mm(15 + 15 + 20, 0.1));
        // The front of the part (y = -40) is on the right here: the rounded corner is the upper right one.
        CHECK_FALSE(drawing::Covers(v.visible, {38, 30}, {40, 30}, kTol));
        CHECK(drawing::Covers(v.visible, {-40, 30}, {35, 30}, kTol));
        CHECK(drawing::Covers(v.visible, {40, 0}, {40, 25}, kTol));
    }
}

TEST_CASE("drawing views: the silhouette of a fillet between two steps of a shaft") {
    // Along X: diameter 40 to x = 60, then diameter 24 to x = 100, with a fillet of 3 in the
    // corner between them — a piece of a torus, with no edge where its outline is seen.
    doc::Document d;
    auto& sketch = AddSketch(d);
    auto& s = sketch.sketch;
    const auto point = [&](Vec2 at) { return s.AddPoint(d.Ids(), at); };
    const ObjectId a = point({0, 0}), b = point({100, 0}), c = point({100, 12}), e = point({63, 12}), f = point({60, 15}),
                   g = point({60, 20}), h = point({0, 20}), centre = point({63, 15});
    s.AddLine(d.Ids(), a, b);
    s.AddLine(d.Ids(), b, c);
    s.AddLine(d.Ids(), c, e);
    s.AddArc(d.Ids(), centre, f, e);   // counter-clockwise from (60, 15) to (63, 12)
    s.AddLine(d.Ids(), f, g);
    s.AddLine(d.Ids(), g, h);
    s.AddLine(d.Ids(), h, a);
    AddRevolve(d, sketch.id, {doc::RevolveAxisKind::SketchX, {}});
    kernel::Model model;
    model.Rebuild(d);
    REQUIRE(AllOk(d));
    WriteSample(model, "views-shaft-fillet");

    const ViewLines v = View(model, ViewProjection::Front);
    const double quarter = kPi * 3 / 2;
    // A millimetre of slack: where the arc runs tangent into the line of the shoulder it is
    // closer to that line than the overlap tolerance for about 0.4 mm, and is drawn by it.
    CHECK(drawing::TotalLength(v.visible) == Mm(2 * 60 + 40 + 40 + 2 * 37 + 2 * quarter + 24, 1.0));
    CHECK(drawing::TotalLength(v.hidden) < kTol);
    // The outline of the fillet: the point of the arc at 45° is on a seen line...
    const double k = 3 * (1 - std::sqrt(0.5));
    const Vec2 onArc{60 + k, 12 + k};
    CHECK(drawing::Covers(v.visible, onArc + Vec2{-0.05, 0.05}, onArc + Vec2{0.05, -0.05}, kTol));
    // ...and where the fillet runs into the thinner step (x = 63) there is no line across the shaft.
    CHECK_FALSE(drawing::Covers(v.visible, {63, -11}, {63, 11}, kTol));
    // From the left the fillet is behind the large end: only the circles of sharp edges, dashed.
    const ViewLines left = View(model, ViewProjection::Left);
    CHECK(drawing::TotalLength(left.visible) == Mm(2 * kPi * 20, 0.1));
    CHECK(drawing::TotalLength(left.hidden) == Mm(2 * kPi * 12, 0.1));
}

TEST_CASE("drawing views: where a round end runs into a flat side there is no line") {
    // A plate with round ends (two half circles of radius 20 joined by straight sides),
    // 10 thick: smooth edges that come from the sketch, not from a fillet.
    doc::Document d;
    auto& sketch = AddSketch(d);
    auto& s = sketch.sketch;
    const ObjectId right = s.AddPoint(d.Ids(), {30, 0}), left = s.AddPoint(d.Ids(), {-30, 0});
    const ObjectId p1 = s.AddPoint(d.Ids(), {30, -20}), p2 = s.AddPoint(d.Ids(), {30, 20});
    const ObjectId p3 = s.AddPoint(d.Ids(), {-30, 20}), p4 = s.AddPoint(d.Ids(), {-30, -20});
    s.AddArc(d.Ids(), right, p1, p2);
    s.AddLine(d.Ids(), p2, p3);
    s.AddArc(d.Ids(), left, p3, p4);
    s.AddLine(d.Ids(), p4, p1);
    AddExtrude(d, sketch.id, "10");
    kernel::Model model;
    model.Rebuild(d);
    REQUIRE(AllOk(d));
    WriteSample(model, "views-round-ends");

    const ViewLines front = View(model, ViewProjection::Front);
    CHECK(drawing::TotalLength(front.visible) == Mm(2 * (100 + 10)));
    CHECK(drawing::TotalLength(front.hidden) < kTol);
    CHECK_FALSE(drawing::Covers(front.visible, {30, 1}, {30, 9}, kTol));    // where the round end begins: no line
    CHECK_FALSE(drawing::Covers(front.visible, {-30, 1}, {-30, 9}, kTol));
    CHECK(drawing::Covers(front.visible, {50, 0}, {50, 10}, kTol));         // the silhouette of the round end
    const ViewLines top = View(model, ViewProjection::Top);
    CHECK(drawing::TotalLength(top.visible) == Mm(2 * 60 + 2 * kPi * 20, 0.1));
    CHECK(drawing::TotalLength(top.hidden) < kTol);
}

TEST_CASE("drawing views: a model without bodies gives an empty view, not an error") {
    doc::Document d;
    kernel::Model model;
    model.Rebuild(d);
    const Expected<ViewLines> lines = kernel::ProjectView(model, kernel::AxesOf(ViewProjection::Front));
    REQUIRE(lines);
    CHECK(lines->Empty());
}
