// A slot for a round glass with a seat cut into its bottom (the tester's
// filter cube, BETA-BUGS №9–№11): fillets on edges that run out into the
// slot's walls, strips a hair wide left between surfaces that almost
// coincide, joins that add nothing.
#include "../Fixtures.h"
#include "document/BzcFile.h"
#include "kernel/Model.h"
#include "kernel/occt/Fillet.h"
#include <doctest.h>

#include <BRepAdaptor_Curve.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <GProp_GProps.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <gp_Ax2.hxx>

#include <cmath>
#include <string>

using namespace bz;
using namespace bz::test;
using doc::FeatureState;

namespace {

double VolumeOf(const TopoDS_Shape& s) {
    GProp_GProps p;
    BRepGProp::VolumeProperties(s, p);
    return p.Mass();
}

/// A block with a slot 25.12 wide, 5.12 thick (y 1…6.12) open at the top,
/// its bottom filled up to a seat: a cylinder of the glass's radius, 12.56,
/// across the slot, tangent to both of its walls.
TopoDS_Shape SeatedSlot() {
    const TopoDS_Shape block = BRepPrimAPI_MakeBox(gp_Pnt(0, 0, 0), gp_Pnt(29.75, 8, 32)).Shape();
    const TopoDS_Shape slot = BRepPrimAPI_MakeBox(gp_Pnt(2.315, 1, 2.79), gp_Pnt(27.435, 6.12, 40)).Shape();
    const TopoDS_Shape lower = BRepPrimAPI_MakeBox(gp_Pnt(2.315, 1, 2.79), gp_Pnt(27.435, 6.12, 15.2)).Shape();
    const TopoDS_Shape glass = BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(14.875, 0, 15.2), gp_Dir(0, 1, 0)), 12.56, 8).Shape();
    const TopoDS_Shape seat = BRepAlgoAPI_Cut(lower, glass).Shape();
    return BRepAlgoAPI_Fuse(BRepAlgoAPI_Cut(block, slot).Shape(), seat).Shape();
}

/// The edges where the seat meets the slot's front wall (y = 1): two arcs.
std::vector<TopoDS_Edge> SeatArcs(const TopoDS_Shape& body, double wallY) {
    std::vector<TopoDS_Edge> arcs;
    for (TopExp_Explorer e(body, TopAbs_EDGE); e.More(); e.Next()) {
        const TopoDS_Edge& edge = TopoDS::Edge(e.Current());
        BRepAdaptor_Curve c(edge);
        if (c.GetType() != GeomAbs_Circle || std::abs(c.Circle().Radius() - 12.56) > 1e-6) continue;
        if (std::abs(c.Value(c.FirstParameter()).Y() - wallY) > 1e-6) continue;
        if (std::ranges::none_of(arcs, [&](const TopoDS_Edge& a) { return a.IsSame(edge); })) arcs.push_back(edge);
    }
    return arcs;
}

} // namespace

TEST_CASE("a fillet on a seat edge that runs out into the slot's wall is built") {
    const TopoDS_Shape body = SeatedSlot();
    REQUIRE(BRepCheck_Analyzer(body).IsValid());
    const std::vector<TopoDS_Edge> arcs = SeatArcs(body, 1.0);
    REQUIRE(arcs.size() == 2);
    // OCCT builds it but leaves the end of the round, at the slot's wall,
    // open by a hair: it was refused as "radius too big" at any radius.
    for (double r : {0.1, 0.3}) {
        CAPTURE(r);
        const Expected<TopoDS_Shape> one = kernel::occt::MakeFillet(body, {arcs.front()}, r);
        REQUIRE_MESSAGE(one, (one ? std::string{} : one.GetError().message));
        CHECK(BRepCheck_Analyzer(*one).IsValid());
        // An inner edge: the round adds material, (1 − π/4)·r² per mm of
        // the arc and of the wall's corner it goes on along (tangent there).
        const double added = VolumeOf(*one) - VolumeOf(body);
        CHECK(added > 0.5 * (1 - kPi / 4) * r * r * 17.8);
        CHECK(added < 2.5 * (1 - kPi / 4) * r * r * 35.0);
    }
    // Both arcs at once: their rounds meet on the 3.9 mm of flat bottom
    // between them. At 0.3 OCCT 7.8 itself faults there (inside
    // ChFi3d_StripeEdgeInter) — in the program that costs the operation
    // only (RunGuarded in Model.cpp), so it is not tried here.
    const Expected<TopoDS_Shape> both = kernel::occt::MakeFillet(body, arcs, 0.1);
    REQUIRE_MESSAGE(both, (both ? std::string{} : both.GetError().message));
    CHECK(BRepCheck_Analyzer(*both).IsValid());
}

TEST_CASE("a fillet on an edge between faces that meet without an angle says so") {
    // The line where the seat's cylinder goes on into the slot's wall.
    const TopoDS_Shape body = SeatedSlot();
    std::optional<TopoDS_Edge> smooth;
    for (TopExp_Explorer e(body, TopAbs_EDGE); e.More() && !smooth; e.Next()) {
        BRepAdaptor_Curve c(TopoDS::Edge(e.Current()));
        const gp_Pnt m = c.Value((c.FirstParameter() + c.LastParameter()) / 2);
        if (c.GetType() == GeomAbs_Line && std::abs(m.X() - 2.315) < 1e-6 && std::abs(m.Z() - 15.2) < 1e-6) smooth = TopoDS::Edge(e.Current());
    }
    REQUIRE(smooth);
    const Expected<TopoDS_Shape> r = kernel::occt::MakeFillet(body, {*smooth}, 0.3);
    REQUIRE_FALSE(r);
    CHECK(r.GetError().message.find("гладкое") != std::string::npos);
}

TEST_CASE("a pocket a hair deeper than the wall over it is reported where it is") {
    // A round pocket 6.15 deep under a slot whose wall is at 6.12: a step of
    // 0.03 mm, a line around the hole that no fillet can cross.
    doc::Document d;
    auto& base = AddSketch(d);
    AddRect(d, base, {0, 0}, {30, 30});
    AddExtrude(d, base.id, "30");
    auto& front = AddSketch(d, doc::origin::kPlaneXZ);
    AddCircle(d, front, {15, 15}, 5);
    auto& pocket = AddExtrude(d, front.id, "6.15", doc::BooleanOp::Cut);
    auto& wall = AddSketch(d, doc::origin::kPlaneXZ);
    AddRect(d, wall, {8, 8}, {22, 40});
    auto& slot = AddExtrude(d, wall.id, "6.12", doc::BooleanOp::Cut);
    kernel::Model model;
    model.Rebuild(d);
    // The pocket goes into the block whichever way the plane looks.
    if (pocket.status.state != FeatureState::Ok) {
        pocket.reversed = slot.reversed = true;
        d.MarkChanged(0);
        model.Rebuild(d);
    }
    REQUIRE(pocket.status.state == FeatureState::Ok);
    CHECK(slot.status.state == FeatureState::Warning);
    CHECK(slot.status.message.find("полоску") != std::string::npos);

    // Made to coincide, it is quiet.
    pocket.distance.expression = "6.12";
    d.MarkChanged(static_cast<std::size_t>(d.IndexOf(pocket.id)));
    model.Rebuild(d);
    CHECK(slot.status.state == FeatureState::Ok);
}

TEST_CASE("a tapered join extruded the other way adds as much as the straight way") {
    // BETA-BUGS №6: with «reversed» and a taper the solid was built on the
    // sketch's other side, inside the body: nothing added, state "ok".
    for (const char* taper : {"0", "10", "45", "-45"})
        for (bool reversed : {false, true}) {
            CAPTURE(taper);
            CAPTURE(reversed);
            doc::Document d;
            auto& base = AddSketch(d);
            AddRect(d, base, {-20, -20}, {20, 20});
            AddExtrude(d, base.id, "40");
            auto& sk = AddSketch(d, doc::origin::kPlaneYZ);
            sk.placement.frame.origin = reversed ? Vec3{-20, 0, 0} : Vec3{20, 0, 0};
            AddRect(d, sk, {-5, 10}, {-1.7, 12});
            AddRect(d, sk, {1.7, 10}, {5, 12});
            auto& boss = AddExtrude(d, sk.id, "0.5", doc::BooleanOp::Join);
            boss.reversed = reversed;
            boss.taper.expression = taper;
            kernel::Model model;
            boss.suppressed = true;
            model.Rebuild(d);
            const double before = model.Volume();
            boss.suppressed = false;
            d.MarkChanged(0);
            model.Rebuild(d);
            CHECK(boss.status.state == FeatureState::Ok);
            CHECK(model.Volume() - before > 4.0);   // 6.6 straight; less or more with a taper
        }
}

TEST_CASE("a join that adds nothing is a warning, not ok") {
    doc::Document d;
    auto& base = AddSketch(d);
    AddRect(d, base, {0, 0}, {20, 20});
    AddExtrude(d, base.id, "20");
    auto& inside = AddSketch(d);
    AddRect(d, inside, {5, 5}, {10, 10});
    auto& join = AddExtrude(d, inside.id, "5", doc::BooleanOp::Join);
    kernel::Model model;
    model.Rebuild(d);
    CHECK(join.status.state == FeatureState::Warning);
    CHECK(join.status.message.find("ничего не изменило") != std::string::npos);
}
