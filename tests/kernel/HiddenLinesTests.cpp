// Lines of drawing views (ADR-0012, step 3): a plate with a hole through it
// seen from the front and from above — where the lines are, which are hidden,
// and the centre lines of the hole.
#include "../Fixtures.h"
#include "document/HoleFeature.h"
#include "drawing/DrawingView.h"
#include "kernel/Model.h"
#include "kernel/drawing/HiddenLines.h"

#include <doctest.h>

#include <cmath>

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

TEST_SUITE_END();
