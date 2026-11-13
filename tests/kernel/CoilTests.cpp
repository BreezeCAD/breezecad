#include "../Fixtures.h"
#include "document/CoilFeature.h"
#include "kernel/Model.h"

#include <doctest.h>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

// The fast set: every pull request runs it (bz_tests -ts=fast, STAB-0 step 4).
TEST_SUITE_BEGIN("fast");

using namespace bz;
using namespace bz::test;
using doc::FeatureState;

namespace {

/// A sketch with an осевая along the sketch Y axis and a round profile beside it.
doc::CoilFeature& AddCoil(doc::Document& d, double profileX, double profileRadius, const std::string& pitch,
                          const std::string& turns) {
    auto& sk = AddSketch(d, doc::origin::kPlaneXZ);
    sketch::Sketch& s = sk.sketch;
    const ObjectId axis = s.AddLine(d.Ids(), s.AddPoint(d.Ids(), {0, -5}), s.AddPoint(d.Ids(), {0, 50}));
    s.FindLine(axis)->centerline = true;
    AddCircle(d, sk, {profileX, 0}, profileRadius);
    auto coil = std::make_unique<doc::CoilFeature>();
    coil->id = d.Ids().Next();
    coil->name = "Пружина1";
    coil->sketch = sk.id;
    coil->axisLine = axis;
    coil->pitch.expression = pitch;
    coil->turns.expression = turns;
    auto* f = coil.get();
    d.Insert(std::move(coil), d.FeatureCount());
    return *f;
}

} // namespace

TEST_CASE("coil winds a sketch profile around its осевая") {
    doc::Document d;
    auto& f = AddCoil(d, 10, 1, "5", "6");
    kernel::Model model;
    model.Rebuild(d);
    REQUIRE(f.status.state == FeatureState::Ok);
    // Section area times the helix length through the profile centre.
    const double helix = 6 * std::hypot(2 * kPi * 10, 5.0);
    CHECK(model.Volume() == doctest::Approx(kPi * 1.0 * helix).epsilon(1e-2));

    f.pitch.expression = "1.5";   // below the profile height: turns would overlap
    d.MarkChanged(1);
    model.Rebuild(d);
    CHECK(f.status.state == FeatureState::Error);

    f.pitch.expression = "5";
    f.axisLine = {};
    d.MarkChanged(1);
    model.Rebuild(d);
    CHECK(f.status.state == FeatureState::Error);   // no axis chosen
}

TEST_CASE("coil profile must not touch the axis") {
    doc::Document d;
    auto& f = AddCoil(d, 0.5, 1, "5", "2");
    kernel::Model model;
    model.Rebuild(d);
    CHECK(f.status.state == FeatureState::Error);
}

namespace {

/// The tester's part in small (BETA-BUGS №4, №5): a block with a Ø22 hole
/// along Z and a «Пружина» cut into the hole's wall — a groove of a
/// triangular section (0.24 deep, 0.48 wide), pitch 0.5. `inner` is the
/// radius of the triangle's tip (inside the hole), `wall` — of its base.
struct GroovedBlock {
    doc::Document     d;
    doc::CoilFeature* coil = nullptr;

    /// `onWall`: a pentagon instead, two of its corners exactly on the
    /// hole's wall (R11) — the profile that cut only a part of the groove.
    GroovedBlock(double turns, bool onWall = false) {
        const double inner = 10.96, wall = 11.2;
        auto& base = AddSketch(d);
        AddRect(d, base, {-20, -20}, {20, 20});
        AddExtrude(d, base.id, "40");
        auto& bore = AddSketch(d);
        AddCircle(d, bore, {0, 0}, 11);
        auto& hole = AddExtrude(d, bore.id, "40", doc::BooleanOp::Cut);
        hole.extent = doc::ExtentType::ThroughAll;

        auto& sk = AddSketch(d, doc::origin::kPlaneXZ);
        sketch::Sketch& s = sk.sketch;
        const ObjectId axis = s.AddLine(d.Ids(), s.AddPoint(d.Ids(), {0, -5}), s.AddPoint(d.Ids(), {0, 50}));
        s.FindLine(axis)->centerline = true;
        std::vector<Vec2> outline = {{inner, 5}, {wall, 5.24}, {wall, 4.76}};
        if (onWall) outline = {{inner, 5}, {11, 5.2}, {wall, 5.24}, {wall, 4.76}, {11, 4.8}};
        std::vector<ObjectId> corners;
        for (Vec2 p : outline) corners.push_back(s.AddPoint(d.Ids(), p));
        for (std::size_t i = 0; i < corners.size(); ++i) s.AddLine(d.Ids(), corners[i], corners[(i + 1) % corners.size()]);
        auto f = std::make_unique<doc::CoilFeature>();
        f->id = d.Ids().Next();
        f->name = "Пружина1";
        f->sketch = sk.id;
        f->axisLine = axis;
        f->operation = doc::BooleanOp::Cut;
        f->pitch.expression = "0.5";
        f->turns.expression = std::to_string(turns);
        coil = f.get();
        d.Insert(std::move(f), d.FeatureCount());
    }

    /// A round pocket from the top, crossing the grooved wall: what every
    /// operation after the «Пружина» costs.
    doc::ExtrudeFeature& AddPocket(double x, const std::string& depth) {
        auto& sk = AddSketch(d);
        sk.placement.frame.origin = {0, 0, 40};
        AddCircle(d, sk, {x, 0}, 3);
        auto& cut = AddExtrude(d, sk.id, depth, doc::BooleanOp::Cut);
        cut.reversed = true;
        return cut;
    }
};

double MsSince(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

} // namespace

// BZ_BENCH_TURNS=28 ./bz_tests -tc="*bench*" -s : the timings of BETA-BUGS №3.
TEST_CASE("bench: a coil cut into a hole's wall and the operations after it") {
    const char* asked = std::getenv("BZ_BENCH_TURNS");
    GroovedBlock part(asked ? std::atof(asked) : 4);
    auto& pocket = part.AddPocket(11, "20");
    kernel::Model model;
    auto t = std::chrono::steady_clock::now();
    model.Rebuild(part.d);
    const double first = MsSince(t);
    REQUIRE(part.coil->status.state != FeatureState::Error);
    REQUIRE(pocket.status.state == FeatureState::Ok);
    const double volume = model.Volume();

    // Only what comes after the coil changes: the cost of one edit below it.
    pocket.distance.expression = "21";
    part.d.MarkChanged(static_cast<std::size_t>(part.d.IndexOf(pocket.id)));
    t = std::chrono::steady_clock::now();
    model.Rebuild(part.d);
    const double after = MsSince(t);
    CHECK(pocket.status.state == FeatureState::Ok);
    CHECK(model.Volume() < volume);
    MESSAGE("coil turns " << part.coil->turns.value << ": whole part " << first << " ms, a cut after the coil " << after << " ms");
}

TEST_CASE("a coil cut whose profile has corners exactly on the hole's wall cuts its groove") {
    // BETA-BUGS №4: the cut came out "ok" with no groove at all (or with a
    // part of it). The boolean is retried with a tolerance for surfaces
    // that only touch; a groove still not cut whole is a warning.
    for (bool onWall : {false, true}) {
        CAPTURE(onWall);
        GroovedBlock part(4, onWall);
        kernel::Model model;
        part.coil->suppressed = true;
        model.Rebuild(part.d);
        const double before = model.Volume();
        part.coil->suppressed = false;
        part.d.MarkChanged(0);
        model.Rebuild(part.d);
        const double removed = before - model.Volume();
        CHECK(part.coil->status.state == FeatureState::Ok);
        // 4 turns of a groove of 0.06 mm² (onWall: 0.07) on a radius of 11.1: 16–20 mm³.
        CHECK(removed > 12.0);
        CHECK(removed < 25.0);
    }
}

TEST_SUITE_END();
