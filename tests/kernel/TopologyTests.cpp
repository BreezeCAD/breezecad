#include "../Fixtures.h"
#include "kernel/Model.h"
#include "kernel/Topology.h"
#include "kernel/occt/FaceMatch.h"
#include "kernel/occt/EdgeMatch.h"
#include "kernel/occt/ModelImpl.h"

#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRep_Builder.hxx>
#include <TopoDS_Compound.hxx>
#include <doctest.h>
#include <gp_Ax2.hxx>
#include <gp_Circ.hxx>

#include <algorithm>
#include <initializer_list>

// The fast set: every pull request runs it (bz_tests -ts=fast, STAB-0 step 4).
TEST_SUITE_BEGIN("fast");

using namespace bz;
using namespace bz::test;

TEST_CASE("topology lists faces and edges with what they are and where") {
    doc::Document d;
    auto& sk = AddSketch(d);
    AddRect(d, sk, {0, 0}, {20, 10});
    AddCircle(d, sk, {10, 5}, 2);   // a hole through
    AddExtrude(d, sk.id, "5");
    kernel::Model model;
    model.Rebuild(d);
    const kernel::Topology t = kernel::DescribeTopology(model);

    // 6 sides + the bore.
    REQUIRE(t.faces.size() == 7);
    const auto planes = std::count_if(t.faces.begin(), t.faces.end(), [](const auto& f) { return f.kind == doc::SurfaceKind::Plane; });
    CHECK(planes == 6);
    const auto bore = std::find_if(t.faces.begin(), t.faces.end(), [](const auto& f) { return f.kind == doc::SurfaceKind::Cylinder; });
    REQUIRE(bore != t.faces.end());
    CHECK(bore->hole);
    REQUIRE(bore->axis);
    CHECK(bore->axis->radius == doctest::Approx(2));
    CHECK(bore->axis->axisDir.Cross({0, 0, 1}).Length() < 1e-9);

    // The top face: normal +Z, area of the plate less the hole, and its reference finds it again.
    const auto top = std::find_if(t.faces.begin(), t.faces.end(), [](const auto& f) {
        return f.kind == doc::SurfaceKind::Plane && f.normal.z > 0.99 && f.point.z > 4.9;
    });
    REQUIRE(top != t.faces.end());
    CHECK(top->area == doctest::Approx(200 - kPi * 4).epsilon(1e-6));
    const auto again = kernel::occt::ResolveFace(model.GetImpl().bodies.front().shape, top->ref);
    CHECK(again.has_value());
    CHECK(top->edges.size() == 5);   // four sides and the hole's rim

    // Edges: 12 of the box, 2 rims and the bore's seam; each line with its length.
    const auto lines = std::count_if(t.edges.begin(), t.edges.end(), [](const auto& e) { return e.kind == doc::CurveKind::Line; });
    const auto circles = std::count_if(t.edges.begin(), t.edges.end(), [](const auto& e) { return e.kind == doc::CurveKind::Circle; });
    CHECK(circles == 2);
    CHECK(lines >= 12);
    const auto rim = std::find_if(t.edges.begin(), t.edges.end(), [](const auto& e) { return e.kind == doc::CurveKind::Circle; });
    CHECK(rim->radius == doctest::Approx(2));
    REQUIRE(rim->centre);
    CHECK(rim->centre->x == doctest::Approx(10));
    CHECK(rim->faces.size() == 2);
    const auto longEdges = std::count_if(t.edges.begin(), t.edges.end(), [](const auto& e) {
        return e.kind == doc::CurveKind::Line && std::abs(e.length - 20) < 1e-6;
    });
    CHECK(longEdges == 4);

    // Distance between the bottom and the top face.
    const auto bottom = std::find_if(t.faces.begin(), t.faces.end(), [](const auto& f) { return f.kind == doc::SurfaceKind::Plane && f.normal.z < -0.99; });
    REQUIRE(bottom != t.faces.end());
    const auto m = kernel::MeasureBetween(model, true, top->index, true, bottom->index);
    REQUIRE(m);
    CHECK(m->distance == doctest::Approx(5));
    CHECK_FALSE(kernel::MeasureBetween(model, true, 99, true, 0));
}

namespace {

const Frame kXyPlane;

TopoDS_Edge HorizontalEdge(double y, double z) {
    return BRepBuilderAPI_MakeEdge(gp_Pnt(-10, y, z), gp_Pnt(10, y, z)).Edge();
}

TopoDS_Compound EdgeCompound(std::initializer_list<TopoDS_Edge> edges) {
    BRep_Builder builder;
    TopoDS_Compound compound;
    builder.MakeCompound(compound);
    for (const auto& edge : edges) builder.Add(compound, edge);
    return compound;
}

} // namespace

TEST_CASE("edge matching without a context plane reports a tie across planes") {
    const TopoDS_Edge below = HorizontalEdge(0, -1);
    const TopoDS_Edge above = HorizontalEdge(0, 1);
    const TopoDS_Compound body = EdgeCompound({below, above});
    doc::EdgeReference ref = kernel::occt::DescribeEdge(below, body);
    ref.middle.z = 0;

    std::vector<bool> ambiguous;
    const auto found = kernel::occt::ResolveEdges({ref}, body, &ambiguous);
    REQUIRE(found.size() == 1);
    CHECK_FALSE(found.front());
    REQUIRE(ambiguous.size() == 1);
    CHECK(ambiguous.front());
}

TEST_CASE("edge matching uses the context plane even when the other edge is closer") {
    const TopoDS_Edge onPlane = HorizontalEdge(0, 0);
    const TopoDS_Edge offPlane = HorizontalEdge(0, 2);
    const TopoDS_Compound body = EdgeCompound({onPlane, offPlane});
    const doc::EdgeReference ref = kernel::occt::DescribeEdge(offPlane, body);

    const auto withoutPlane = kernel::occt::ResolveEdges({ref}, body);
    REQUIRE(withoutPlane.front());
    CHECK(withoutPlane.front()->IsSame(offPlane));

    std::vector<bool> ambiguous;
    const auto found = kernel::occt::ResolveEdges({ref}, body, &ambiguous, &kXyPlane);
    REQUIRE(found.front());
    CHECK(found.front()->IsSame(onPlane));
    REQUIRE(ambiguous.size() == 1);
    CHECK_FALSE(ambiguous.front());
}

TEST_CASE("a context plane does not resolve a tie between two coplanar edges") {
    const TopoDS_Edge left = HorizontalEdge(-1, 0);
    const TopoDS_Edge right = HorizontalEdge(1, 0);
    const TopoDS_Compound body = EdgeCompound({left, right});
    doc::EdgeReference ref = kernel::occt::DescribeEdge(left, body);
    ref.middle.y = 0;

    std::vector<bool> ambiguous;
    const auto found = kernel::occt::ResolveEdges({ref}, body, &ambiguous, &kXyPlane);
    REQUIRE(found.size() == 1);
    CHECK_FALSE(found.front());
    REQUIRE(ambiguous.size() == 1);
    CHECK(ambiguous.front());
}

TEST_CASE("an edge crossing the context plane at its midpoint is not coplanar") {
    const TopoDS_Edge original = HorizontalEdge(0, 0);
    const TopoDS_Compound originalBody = EdgeCompound({original});
    const doc::EdgeReference ref = kernel::occt::DescribeEdge(original, originalBody);
    const TopoDS_Edge tilted = BRepBuilderAPI_MakeEdge(gp_Pnt(-10, 0, -0.1), gp_Pnt(10, 0, 0.1)).Edge();
    const TopoDS_Compound body = EdgeCompound({tilted});

    const auto withoutPlane = kernel::occt::ResolveEdges({ref}, body);
    REQUIRE(withoutPlane.front());
    CHECK(withoutPlane.front()->IsSame(tilted));

    std::vector<bool> ambiguous;
    const auto found = kernel::occt::ResolveEdges({ref}, body, &ambiguous, &kXyPlane);
    REQUIRE(found.size() == 1);
    CHECK_FALSE(found.front());
    REQUIRE(ambiguous.size() == 1);
    CHECK_FALSE(ambiguous.front());
}

TEST_CASE("circle matching follows only the context plane and preserves a lost reference") {
    const TopoDS_Edge onPlane = BRepBuilderAPI_MakeEdge(gp_Circ(gp_Ax2(gp_Pnt(3, 4, 0), gp_Dir(0, 0, 1)), 5)).Edge();
    const TopoDS_Edge offPlane = BRepBuilderAPI_MakeEdge(gp_Circ(gp_Ax2(gp_Pnt(3, 4, 2), gp_Dir(0, 0, 1)), 5)).Edge();
    const TopoDS_Compound body = EdgeCompound({onPlane, offPlane});
    doc::EdgeReference ref = kernel::occt::DescribeEdge(offPlane, body);

    const auto found = kernel::occt::ResolveCircle(ref, body, &kXyPlane);
    REQUIRE(found);
    CHECK(found->first == Vec3{3, 4, 0});
    CHECK(ref == kernel::occt::DescribeEdge(onPlane, body));

    const doc::EdgeReference saved = ref;
    const TopoDS_Edge tilted =
        BRepBuilderAPI_MakeEdge(gp_Circ(gp_Ax2(gp_Pnt(3, 4, 0), gp_Dir(0.01, 0, 1), gp_Dir(0, 1, 0)), 5)).Edge();
    const TopoDS_Compound rebuilt = EdgeCompound({offPlane, tilted});
    CHECK_FALSE(kernel::occt::ResolveCircle(ref, rebuilt, &kXyPlane));
    CHECK(ref == saved);
}

TEST_SUITE_END();
