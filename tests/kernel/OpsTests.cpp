// The one pipeline of booleans (kernel/occt/Ops.h, STAB-3): what it gives
// back, how it falls back, and the measurements its constants come from.

#include "kernel/occt/Ops.h"

#include <doctest.h>

#include <BRepAlgoAPI_Cut.hxx>   // the measurements below compare with a bare boolean (tests are not src/)
#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakeHalfSpace.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <GProp_GProps.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <gp_Pln.hxx>

#include <chrono>
#include <cmath>

using namespace bz;
using namespace bz::kernel::occt;

namespace {

double Volume(const TopoDS_Shape& s) {
    GProp_GProps p;
    BRepGProp::VolumeProperties(s, p);
    return p.Mass();
}

int Solids(const TopoDS_Shape& s) {
    int n = 0;
    for (TopExp_Explorer e(s, TopAbs_SOLID); e.More(); e.Next()) ++n;
    return n;
}

TopoDS_Shape Box(double x0, double y0, double z0, double x1, double y1, double z1) {
    return BRepPrimAPI_MakeBox(gp_Pnt(x0, y0, z0), gp_Pnt(x1, y1, z1)).Shape();
}

/// A 10 mm cube with a slot cut from one side, leaving a wall `web` thick
/// next to the place a cut from the other side will go: the web of a thread
/// between two grooves, in plain boxes.
struct Web {
    TopoDS_Shape body, tool;
    explicit Web(double web) {
        const double gap = web / 2;
        body = *ops::Cut(Box(0, 0, 0, 10, 10, 10), Box(-1, 1, 1, 5 - gap, 9, 11));
        tool = Box(5 + gap, 1, 1, 11, 9, 11);
    }
};

} // namespace

TEST_SUITE_BEGIN("fast");

TEST_CASE("ops: an exact boolean is checked and says Ok") {
    const ops::Result r = ops::Boolean(ops::Kind::Cut, Box(0, 0, 0, 10, 10, 10), Box(2, 2, -1, 4, 4, 11));
    REQUIRE(r.Usable());
    CHECK(r.code == ops::Code::Ok);
    CHECK(r.message.empty());
    CHECK(r.after == doctest::Approx(1000 - 40));
    CHECK(r.before == doctest::Approx(1000));
}

TEST_CASE("ops: a cut that misses comes back as it was, a cut of everything as empty") {
    const TopoDS_Shape cube = Box(0, 0, 0, 10, 10, 10);
    const ops::Result missed = ops::Boolean(ops::Kind::Cut, cube, Box(20, 20, 20, 30, 30, 30));
    CHECK(missed.code == ops::Code::Missed);
    CHECK(missed.Usable());
    CHECK(Volume(missed.shape) == doctest::Approx(1000));

    const ops::Result all = ops::Boolean(ops::Kind::Cut, cube, Box(-1, -1, -1, 11, 11, 11));
    CHECK(all.code == ops::Code::Empty);
    CHECK_FALSE(all.Usable());
    CHECK(all.message == "После операции не осталось материала");

    const ops::Result none = ops::Boolean(ops::Kind::Common, cube, Box(20, 20, 20, 30, 30, 30));
    CHECK(none.code == ops::Code::Empty);
    CHECK(none.message == "Пересечение пустое: тела не перекрываются");
}

TEST_CASE("ops: an intersection with a half-space is not judged by its volume") {
    // A half-space has no finite volume: the bound "no more than the tool" is none.
    const TopoDS_Face plane = BRepBuilderAPI_MakeFace(gp_Pln(gp_Pnt(0, 0, 4), gp_Dir(0, 0, 1))).Face();
    const TopoDS_Shape half = BRepPrimAPI_MakeHalfSpace(plane, gp_Pnt(0, 0, 0)).Solid();
    const ops::Result r = ops::Boolean(ops::Kind::Common, Box(0, 0, 0, 10, 10, 10), half);
    REQUIRE(r.Usable());
    CHECK(Volume(r.shape) == doctest::Approx(400));
}

TEST_CASE("ops: a deadline stops a boolean and says so") {
    // Many cylinders through a cube: far more than nothing to do.
    TopoDS_Shape tools;
    std::vector<TopoDS_Shape> list;
    for (int i = 0; i < 20; ++i)
        for (int j = 0; j < 20; ++j)
            list.push_back(BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(0.25 + i * 0.5, 0.25 + j * 0.5, -1), gp::DZ()), 0.2, 12).Shape());
    ops::Options o;
    o.seconds = 1e-4;
    const ops::Result r = ops::Boolean(ops::Kind::Cut, Box(0, 0, 0, 10, 10, 10), list, o);
    CHECK(r.code == ops::Code::Timeout);
    CHECK_FALSE(r.Usable());
    CHECK(r.message.find("не уложилась") != std::string::npos);
    CHECK(r.ms < 2000);
}

TEST_CASE("ops: a web as thin as 0.02 mm survives the touching tolerance") {
    // The largest of kFuzzySteps must not eat the web of a thread (the
    // 1e-2 of the first fallbacks did: STAB-3).
    for (double fuzzy : ops::kFuzzySteps) {
        CAPTURE(fuzzy);
        const Web w(0.02);
        ops::Options o;
        o.exactFirst = false;
        o.fuzzySteps = {fuzzy};
        o.glue = false;
        const ops::Result r = ops::Boolean(ops::Kind::Cut, w.body, w.tool, o);
        REQUIRE(r.Usable());
        CHECK(Solids(r.shape) == 1);
        const double webVolume = 0.02 * 8 * 9;   // across, along y, up z
        const double expected = 1000 - 2 * (5 - 0.01) * 8 * 9;
        CHECK(std::abs(Volume(r.shape) - expected) < 0.05 * webVolume);
    }
}

TEST_CASE("ops: an Error, not OCCT's words, when a shorthand fails") {
    const Expected<TopoDS_Shape> r = ops::Fuse(TopoDS_Shape(), Box(0, 0, 0, 1, 1, 1));
    REQUIRE_FALSE(r);
    CHECK(r.GetError().message.find("Не удалось выполнить") == 0);
}

TEST_SUITE_END();

// Measurements behind kFuzzySteps (docs/dev/OCCT-ISSUES.md): what each
// tolerance does to a thin web. Run: bz_tests -ts=measure -s
TEST_CASE("measure: touching tolerance against a thin web" * doctest::test_suite("measure")) {
    for (double web : {0.005, 0.01, 0.02, 0.05}) {
        for (double fuzzy : {0.0, 1e-6, 1e-5, 1e-4, 1e-3, 5e-3, 1e-2}) {
            const Web w(web);
            ops::Options o;
            o.exactFirst = fuzzy == 0.0;
            o.fuzzySteps = {fuzzy};
            o.retry = fuzzy > 0.0;
            o.glue = false;
            o.verify = false;
            const auto t = std::chrono::steady_clock::now();
            const ops::Result r = ops::Boolean(ops::Kind::Cut, w.body, w.tool, o);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
            const double expected = 1000 - 2 * (5 - web / 2) * 8 * 9;   // the cube less the slot and the cut, inside it
            MESSAGE("web " << web << " fuzzy " << fuzzy << ": code " << static_cast<int>(r.code) << ", solids "
                           << (r.Usable() ? Solids(r.shape) : -1) << ", volume error "
                           << (r.Usable() ? Volume(r.shape) - expected : 0.0) << " mm3, " << ms << " ms");
        }
    }
}
