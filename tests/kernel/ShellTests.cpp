#include "../Fixtures.h"
#include "document/ShellFeature.h"
#include "kernel/Model.h"

#include <doctest.h>

// The fast set: every pull request runs it (bz_tests -ts=fast, STAB-0 step 4).
TEST_SUITE_BEGIN("fast");

using namespace bz;
using namespace bz::test;
using doc::FeatureState;

namespace {

/// A 20 mm cube from z = 0 to 20 with a shell after it.
struct Cube {
    doc::Document      d;
    doc::ShellFeature* shell = nullptr;
    doc::ExtrudeFeature* extrude = nullptr;
    explicit Cube(const std::string& thickness, bool openTop, bool outward = false) {
        auto& sk = AddSketch(d);
        AddRect(d, sk, {-10, -10}, {10, 10});
        extrude = &AddExtrude(d, sk.id, "20");
        auto f = std::make_unique<doc::ShellFeature>();
        f->id = d.Ids().Next();
        f->name = d.UniqueFeatureName("Оболочка");
        f->thickness.expression = thickness;
        f->outward = outward;
        if (openTop) f->faces.push_back(doc::FaceReference{{0, 0, 1}, {0, 0, 20}});
        shell = f.get();
        d.Insert(std::move(f), d.FeatureCount());
    }
};

} // namespace

TEST_CASE("shell with the top removed leaves a box open at the top") {
    Cube c("2", true);
    kernel::Model model;
    model.Rebuild(c.d);
    REQUIRE(c.shell->status.state == FeatureState::Ok);
    CHECK(model.Volume() == doctest::Approx(8000.0 - 16.0 * 16.0 * 18.0).epsilon(1e-6));
}

TEST_CASE("shell without removed faces is closed and hollow") {
    Cube c("2", false);
    kernel::Model model;
    model.Rebuild(c.d);
    REQUIRE(c.shell->status.state == FeatureState::Ok);
    CHECK(model.Volume() == doctest::Approx(8000.0 - 16.0 * 16.0 * 16.0).epsilon(1e-6));
}

TEST_CASE("outward shell keeps the inside size") {
    Cube c("2", true, true);
    kernel::Model model;
    model.Rebuild(c.d);
    REQUIRE(c.shell->status.state == FeatureState::Ok);
    CHECK(model.Volume() == doctest::Approx(24.0 * 24.0 * 22.0 - 8000.0).epsilon(1e-6));
}

TEST_CASE("shell follows the top face when the cube gets taller") {
    Cube c("2", true);
    kernel::Model model;
    model.Rebuild(c.d);
    c.extrude->distance.expression = "30";
    c.d.MarkChanged(0);
    model.Rebuild(c.d);
    REQUIRE(c.shell->status.state == FeatureState::Ok);
    CHECK(model.Volume() == doctest::Approx(20.0 * 20.0 * 30.0 - 16.0 * 16.0 * 28.0).epsilon(1e-6));
}

TEST_CASE("too thick a shell is an error, not a crash") {
    Cube c("15", true);
    kernel::Model model;
    model.Rebuild(c.d);
    CHECK(c.shell->status.state == FeatureState::Error);
}

// The slot ends 2 mm from the outer wall, so with a 2 mm shell its tube
// touches the wall: the OCCT offset fails there and the shell is made of
// booleans (ShellBySlabs).
TEST_CASE("shell open at the top of a plate with a through hole and a slot: they get tube walls") {
    // Found on Windows (MCP): a 60x40x12 plate with a hole, shell 2 mm open
    // at the top failed with "too thick". Inventor builds it: the hole
    // becomes a tube with a 2 mm wall, the floor is 2 mm thick.
    doc::Document d;
    auto& sk = AddSketch(d);
    AddRect(d, sk, {0, 0}, {60, 40});
    AddCircle(d, sk, {20, 20}, 5);
    {   // and a slot 38..52 x 16..24, as drawn in the Windows run
        IdGenerator& ids = d.Ids();
        sketch::Sketch& s = sk.sketch;
        const ObjectId c1 = s.AddPoint(ids, {38, 20}), c2 = s.AddPoint(ids, {52, 20});
        const ObjectId a = s.AddPoint(ids, {38, 16}), b = s.AddPoint(ids, {52, 16});
        const ObjectId c = s.AddPoint(ids, {52, 24}), e = s.AddPoint(ids, {38, 24});
        s.AddLine(ids, a, b);
        s.AddArc(ids, c2, b, c);
        s.AddLine(ids, c, e);
        s.AddArc(ids, c1, e, a);
    }
    AddExtrude(d, sk.id, "12");
    auto f = std::make_unique<doc::ShellFeature>();
    f->id = d.Ids().Next();
    f->name = d.UniqueFeatureName("Оболочка");
    f->thickness.expression = "2";
    f->faces.push_back(doc::FaceReference{{0, 0, 1}, {40, 30, 12}});
    doc::ShellFeature* shell = f.get();
    d.Insert(std::move(f), d.FeatureCount());
    kernel::Model model;
    model.Rebuild(d);
    INFO(shell->status.message);
    REQUIRE(shell->status.state == FeatureState::Ok);
    // Floor 56x36x2 less the hole, outer walls, the tube r 5..7 above the floor.
    const double pi = 3.14159265358979;
    const double slot = 14.0 * 8 + pi * 16, slotWall = 14.0 * 12 + pi * 36;
    const double whole = 60.0 * 40 * 12 - pi * 25 * 12 - slot * 12;
    const double cavity = 56.0 * 36 * 10 - pi * 49 * 10 - slotWall * 10;
    CHECK(model.Volume() == doctest::Approx(whole - cavity).epsilon(1e-4));
}

TEST_SUITE_END();
