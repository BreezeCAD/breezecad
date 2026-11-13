// Scenarios where an edit changes the topology the later operations refer to
// (stab/fc-ideas, after FreeCAD's TestTopologicalNamingProblem — the ideas,
// not their code). Each states what Inventor does; a scenario we do not
// handle yet is marked may_fail: it documents the gap and starts passing
// when references learn the history of the model.
//
// Not in the fast set: these are a catalogue, not a guard of today's code.

#include "../Fixtures.h"
#include "document/EdgeFeature.h"
#include "kernel/Model.h"

#include <doctest.h>

#include <cmath>

TEST_SUITE_BEGIN("topology");

using namespace bz;
using namespace bz::test;
using doc::CurveKind;
using doc::EdgeReference;
using doc::FeatureState;
using doc::SurfaceKind;

namespace {

/// The top edge of a hole of `radius` at (x, y) through a box 30 high.
EdgeReference HoleTopEdge(double x, double y, double radius) {
    EdgeReference e;
    e.curve = CurveKind::Circle;
    e.direction = {0, 0, 1};
    e.radius = radius;
    e.middle = {x - radius, y, 30};   // the middle of a full circle starting at angle 0
    e.faces = {{{SurfaceKind::Plane, {0, 0, 1}}, {SurfaceKind::Cylinder, {0, 0, 1}}}};
    return e;
}

/// The top front edge of a box from z = 0 to 30, front face at y = -40.
EdgeReference TopFrontEdge() {
    EdgeReference e;
    e.curve = CurveKind::Line;
    e.direction = {1, 0, 0};
    e.middle = {0, -40, 30};
    e.faces = {{{SurfaceKind::Plane, {0, -1, 0}}, {SurfaceKind::Plane, {0, 0, 1}}}};
    return e;
}

template <class F>
F& AddEdgeFeature(doc::Document& d, std::vector<EdgeReference> edges, const std::string& size) {
    auto f = std::make_unique<F>();
    f->id = d.Ids().Next();
    f->name = d.UniqueFeatureName("Операция");
    f->edges = std::move(edges);
    std::vector<Parameter*> params;
    f->CollectParameters(params);
    for (Parameter* p : params) p->expression = size;
    auto* raw = f.get();
    d.Insert(std::move(f), d.FeatureCount());
    return *raw;
}

/// Moves the point of `sketch` that sits at `from` to `to`.
void MovePoint(doc::SketchFeature& sketch, Vec2 from, Vec2 to) {
    for (auto& p : sketch.sketch.points)
        if (!p.isOrigin && std::abs(p.pos.x - from.x) < 1e-9 && std::abs(p.pos.y - from.y) < 1e-9) p.pos = to;
}

} // namespace

// Two equal holes; the right one is chamfered. Then the holes trade places:
// the chamfered one goes far left, the other comes near where the chamfered
// one was. Inventor: the chamfer stays on its own hole (it was made from
// that circle). We look for the edge nearest the old place — the other hole.
TEST_CASE("a chamfered hole that moves keeps its chamfer, not its neighbour" * doctest::may_fail()) {
    doc::Document d;
    auto& base = AddSketch(d);
    AddRect(d, base, {-60, -40}, {60, 40});
    AddExtrude(d, base.id, "30");
    auto& holes = AddSketch(d);
    AddCircle(d, holes, {-30, 0}, 5);
    AddCircle(d, holes, {30, 0}, 5);
    AddExtrude(d, holes.id, "30", doc::BooleanOp::Cut);
    auto& chamfer = AddEdgeFeature<doc::ChamferFeature>(d, {HoleTopEdge(30, 0, 5)}, "2");

    kernel::Model model;
    model.Rebuild(d);
    REQUIRE(chamfer.status.state == FeatureState::Ok);

    MovePoint(holes, {30, 0}, {-45, 0});   // the chamfered hole, far left
    MovePoint(holes, {-30, 0}, {25, 0});   // the other one, next to the old place
    d.MarkChanged(2);
    model.Rebuild(d);
    REQUIRE(chamfer.status.state == FeatureState::Ok);
    // Material taken at x = -45 moves the centre of mass right of where it
    // is without the chamfer.
    const double with = model.Properties()->centre.x;
    chamfer.suppressed = true;
    d.MarkChanged(4);
    model.Rebuild(d);
    CHECK(with > model.Properties()->centre.x);
}

// A fillet on the top front edge; then a slot is added before it that cuts
// through the middle of that edge. Inventor: both remaining pieces of the
// edge stay rounded (each is a piece of the edge the fillet was made on).
// We (13.11.2026): the fillet silently moves to the top edge of the slot's
// back wall — it faces the same two ways and its middle is the nearest to
// the old one. Status Ok, about 10 mm rounded instead of 110.
TEST_CASE("a filleted edge split by an earlier slot keeps the fillet on both pieces" * doctest::may_fail()) {
    doc::Document d;
    auto& base = AddSketch(d);
    AddRect(d, base, {-60, -40}, {60, 40});
    AddExtrude(d, base.id, "30");
    auto& fillet = AddEdgeFeature<doc::FilletFeature>(d, {TopFrontEdge()}, "5");
    kernel::Model model;
    model.Rebuild(d);
    REQUIRE(fillet.status.state == FeatureState::Ok);

    // The slot goes in right after the box (before the fillet).
    auto slot = std::make_unique<doc::SketchFeature>();
    slot->id = d.Ids().Next();
    slot->name = d.UniqueFeatureName("Эскиз");
    slot->placement = {doc::origin::kPlaneXY, doc::origin::PlaneFrame(doc::origin::kPlaneXY).value_or(Frame{}), false, {}};
    slot->sketch.EnsureOrigin(d.Ids());
    auto* slotSketch = slot.get();
    d.Insert(std::move(slot), 2);
    AddRect(d, *slotSketch, {-5, -50}, {5, -30});
    auto cut = std::make_unique<doc::ExtrudeFeature>();
    cut->id = d.Ids().Next();
    cut->name = d.UniqueFeatureName("Выдавливание");
    cut->sketch = slotSketch->id;
    cut->operation = doc::BooleanOp::Cut;
    cut->distance.name = d.NextParameterName();
    cut->distance.expression = "30";
    d.Insert(std::move(cut), 3);
    d.MarkChanged(2);
    model.Rebuild(d);

    REQUIRE(fillet.status.state != FeatureState::Error);
    const double box = 120.0 * 80 * 30, slotVolume = 10.0 * 10 * 30;
    const double rounded = (1 - kPi / 4) * 25 * 110;   // both pieces: 2 x 55 mm
    CHECK(model.Volume() == doctest::Approx(box - slotVolume - rounded).epsilon(1e-4));
}

// A sketch on the top face of a boss; then the boss is suppressed, so that
// face is gone. Inventor: the sketch is "sick" — it says its face is lost.
// We take the nearest face facing the same way (the plate's top, 10 mm
// lower) and say nothing.
TEST_CASE("a sketch whose face is gone says so instead of moving to another face" * doctest::may_fail()) {
    doc::Document d;
    auto& base = AddSketch(d);
    AddRect(d, base, {0, 0}, {40, 40});
    AddExtrude(d, base.id, "10");
    auto& bossSketch = AddSketch(d);
    bossSketch.placement = {d.Features()[1]->id, {{0, 0, 10}, {1, 0, 0}, {0, 0, 1}}, true, {{0, 0, 1}, {20, 20, 10}}};
    AddRect(d, bossSketch, {0, 0}, {20, 20});
    auto& boss = AddExtrude(d, bossSketch.id, "10", doc::BooleanOp::Join);
    auto& pin = AddSketch(d);
    pin.placement = {boss.id, {{0, 0, 20}, {1, 0, 0}, {0, 0, 1}}, true, {{0, 0, 1}, {10, 10, 20}}};
    AddCircle(d, pin, {10, 10}, 3);
    AddExtrude(d, pin.id, "5", doc::BooleanOp::Join);
    kernel::Model model;
    model.Rebuild(d);
    REQUIRE(pin.status.state == FeatureState::Ok);
    REQUIRE(model.BoundingBox()->max.z == doctest::Approx(25.0).epsilon(1e-4));

    boss.suppressed = true;
    d.MarkChanged(3);
    model.Rebuild(d);
    CHECK(pin.status.state != FeatureState::Ok);
}

// The control: what already works must keep working. The holes only move
// apart (no swap): the chamfer stays on the right hole.
TEST_CASE("a chamfered hole that moves a little keeps its chamfer") {
    doc::Document d;
    auto& base = AddSketch(d);
    AddRect(d, base, {-60, -40}, {60, 40});
    AddExtrude(d, base.id, "30");
    auto& holes = AddSketch(d);
    AddCircle(d, holes, {-30, 0}, 5);
    AddCircle(d, holes, {30, 0}, 5);
    AddExtrude(d, holes.id, "30", doc::BooleanOp::Cut);
    auto& chamfer = AddEdgeFeature<doc::ChamferFeature>(d, {HoleTopEdge(30, 0, 5)}, "2");
    kernel::Model model;
    model.Rebuild(d);
    REQUIRE(chamfer.status.state == FeatureState::Ok);

    MovePoint(holes, {30, 0}, {40, 0});
    MovePoint(holes, {-30, 0}, {-40, 0});
    d.MarkChanged(2);
    model.Rebuild(d);
    REQUIRE(chamfer.status.state == FeatureState::Ok);
    const double with = model.Properties()->centre.x;
    chamfer.suppressed = true;
    d.MarkChanged(4);
    model.Rebuild(d);
    CHECK(with < model.Properties()->centre.x);   // material taken at x = +40
}

TEST_SUITE_END();
