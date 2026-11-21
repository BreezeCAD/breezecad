// The one pipeline of booleans (kernel/occt/Ops.h, STAB-3): what it gives
// back, how it falls back, and the measurements its constants come from.

#include "kernel/occt/Ops.h"

#include <doctest.h>

#include <BRepAlgoAPI_Common.hxx>   // the OCCT cases below compare with a bare boolean (tests are not src/)
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepTools.hxx>
#include <BRep_Builder.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Iterator.hxx>
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
#include <cstdlib>
#include <filesystem>
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

// --- Cases of OCCT itself (docs/dev/OCCT-ISSUES.md) -------------------------
// Each is a minimal .brep in tests/data/occt/ and a test that shows what
// OCCT does with it bare and what the pipeline makes of it. When OCCT is
// updated (BreezeCAD/occt), a bare check that starts failing means OCCT
// changed there: look again whether the workaround is still needed.

namespace {

std::filesystem::path OcctData(const char* name) { return std::filesystem::path(BZ_SOURCE_DIR) / "tests/data/occt" / name; }

/// The shapes of a compound written by the tests below: target first, then tools.
std::vector<TopoDS_Shape> ReadCase(const char* name) {
    TopoDS_Shape all;
    BRep_Builder builder;
    REQUIRE(BRepTools::Read(all, OcctData(name).string().c_str(), builder));
    std::vector<TopoDS_Shape> out;
    for (TopoDS_Iterator it(all); it.More(); it.Next()) out.push_back(it.Value());
    return out;
}

} // namespace

TEST_CASE("occt: a glued intersection with a half-space gives a wrong body" * doctest::test_suite("fast")) {
    // OCCT-ISSUES №1. A box 10³ and the half-space z < 4.
    const std::vector<TopoDS_Shape> shapes = ReadCase("glue-halfspace-common.brep");
    REQUIRE(shapes.size() == 2);
    BRepAlgoAPI_Common glued;
    NCollection_List<TopoDS_Shape> a, b;
    a.Append(shapes[0]);
    b.Append(shapes[1]);
    glued.SetArguments(a);
    glued.SetTools(b);
    glued.SetGlue(BOPAlgo_GlueShift);
    glued.Build();
    REQUIRE(glued.IsDone());
    CHECK(Volume(glued.Shape()) != doctest::Approx(400));   // OCCT 8.0.1: 200 000
    // The pipeline tries the exact one first, and checks what any attempt gives.
    const ops::Result r = ops::Boolean(ops::Kind::Common, shapes[0], shapes[1]);
    REQUIRE(r.Usable());
    CHECK(Volume(r.shape) == doctest::Approx(400));
}

// Writes the .brep files above from the shapes the tests build:
// BZ_OCCT_WRITE=1 bz_tests -ts=write-occt
TEST_CASE("write the OCCT cases" * doctest::test_suite("write-occt")) {
    if (!std::getenv("BZ_OCCT_WRITE")) return;
    std::filesystem::create_directories(OcctData(""));
    const auto write = [](const char* name, std::initializer_list<TopoDS_Shape> shapes) {
        TopoDS_Compound all;
        BRep_Builder builder;
        builder.MakeCompound(all);
        for (const TopoDS_Shape& s : shapes) builder.Add(all, s);
        REQUIRE(BRepTools::Write(all, OcctData(name).string().c_str()));
    };
    const TopoDS_Face plane = BRepBuilderAPI_MakeFace(gp_Pln(gp_Pnt(0, 0, 4), gp_Dir(0, 0, 1))).Face();
    write("glue-halfspace-common.brep", {Box(0, 0, 0, 10, 10, 10), BRepPrimAPI_MakeHalfSpace(plane, gp_Pnt(0, 0, 0)).Solid()});
}
