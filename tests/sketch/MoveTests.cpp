#include "sketch/Move.h"
#include "sketch/Shapes.h"
#include "sketch/Solver.h"

#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <vector>

using namespace bz;
using namespace bz::sketch;

TEST_CASE("move a rectangle and rotate it, it stays a rectangle") {
    Sketch sk;
    IdGenerator ids;
    const RectangleIds r = AddCornerRectangle(sk, ids, sk.AddPoint(ids, {0, 0}), sk.AddPoint(ids, {40, 20}));
    const std::vector<ObjectId> all(r.sides.begin(), r.sides.end());

    REQUIRE(MoveEntities(sk, ids, all, {{0, 0}, 0, {10, 5}}));
    REQUIRE(Solve(sk).converged);
    CHECK(sk.PositionOf(r.corners[0]).x == doctest::Approx(10.0));
    CHECK(sk.PositionOf(r.corners[2]).y == doctest::Approx(25.0));

    REQUIRE(MoveEntities(sk, ids, all, {sk.PositionOf(r.corners[0]), kPi / 6, {}}));
    REQUIRE(Solve(sk).converged);
    const Vec2 s0 = sk.PositionOf(r.corners[1]) - sk.PositionOf(r.corners[0]);
    const Vec2 s1 = sk.PositionOf(r.corners[2]) - sk.PositionOf(r.corners[1]);
    CHECK(std::atan2(s0.y, s0.x) == doctest::Approx(kPi / 6));   // turned, not snapped back
    CHECK(s0.Dot(s1) == doctest::Approx(0.0).epsilon(1e-6));
    CHECK(s0.Length() == doctest::Approx(40.0));
}

TEST_CASE("the origin stays put") {
    Sketch sk;
    IdGenerator ids;
    const ObjectId origin = sk.EnsureOrigin(ids);
    const ObjectId line = sk.AddLine(ids, origin, sk.AddPoint(ids, {10, 0}));
    const ObjectId lone = line;
    REQUIRE(MoveEntities(sk, ids, std::span(&lone, 1), {{0, 0}, 0, {5, 5}}));
    CHECK(sk.PositionOf(origin).x == doctest::Approx(0.0));
    CHECK(sk.PositionOf(sk.FindLine(line)->end).x == doctest::Approx(15.0));
}

TEST_CASE("copies keep their shape and follow the original's size") {
    Sketch sk;
    IdGenerator ids;
    const RectangleIds r = AddCornerRectangle(sk, ids, sk.AddPoint(ids, {0, 0}), sk.AddPoint(ids, {10, 5}));
    Constraint len;
    len.type = ConstraintType::Length;
    len.refs = {r.sides[0], {}, {}};
    len.parameter.value = 10;
    sk.AddConstraint(ids, len);
    const std::vector<ObjectId> all(r.sides.begin(), r.sides.end());
    const auto copies = CopyEntities(sk, ids, all, {{0, 0}, 0, {20, 0}});
    REQUIRE(copies.size() == 4);
    for (Constraint& c : sk.constraints)
        if (c.type == ConstraintType::Length) c.parameter.value = 14;
    REQUIRE(Solve(sk).converged);
    const Line* copy = sk.FindLine(copies[0]);
    CHECK((sk.PositionOf(copy->end) - sk.PositionOf(copy->start)).Length() == doctest::Approx(14.0));
}

TEST_CASE("scaling a circle and a line about a point") {
    Sketch sk;
    IdGenerator ids;
    const ObjectId circle = sk.AddCircle(ids, sk.AddPoint(ids, {10, 0}), 3);
    const ObjectId line = sk.AddLine(ids, sk.AddPoint(ids, {0, 0}), sk.AddPoint(ids, {4, 0}));
    const ObjectId chosen[] = {circle, line};
    REQUIRE(ScaleEntities(sk, chosen, {0, 0}, 2));
    CHECK(sk.RadiusOf(circle) == doctest::Approx(6.0));
    CHECK(sk.PositionOf(sk.FindCircle(circle)->center).x == doctest::Approx(20.0));
    CHECK(sk.PositionOf(sk.FindLine(line)->end).x == doctest::Approx(8.0));
}

TEST_CASE("a spline moves and copies with its points") {
    Sketch sk;
    IdGenerator ids;
    std::vector<ObjectId> pts;
    for (const Vec2 p : {Vec2{0, 0}, Vec2{10, 5}, Vec2{20, 0}}) pts.push_back(sk.AddPoint(ids, p));
    pts.push_back(pts.front());
    const ObjectId spline = sk.AddSpline(ids, pts, true);
    const std::vector<ObjectId> chosen{spline};
    CHECK(MovedPoints(sk, chosen).size() == 3);
    CopyEntities(sk, ids, chosen, Motion{{}, 0, {100, 0}});
    REQUIRE(sk.splines.size() == 2);
    const Spline& copy = sk.splines.back();
    CHECK(copy.Closed());
    CHECK(sk.PositionOf(copy.points[1]).x == doctest::Approx(110.0));
}

namespace {

int CountOf(const Sketch& sk, ConstraintType type) {
    int n = 0;
    for (const Constraint& c : sk.constraints) n += c.type == type;
    return n;
}

double SideLength(const Sketch& sk, ObjectId line) {
    const Line* l = sk.FindLine(line);
    return (sk.PositionOf(l->end) - sk.PositionOf(l->start)).Length();
}

} // namespace

TEST_CASE("a quarter turn swaps horizontal and vertical, the rectangle keeps its sides") {
    for (const double angle : {kPi / 2, -kPi / 2, 3 * kPi / 2}) {
        Sketch sk;
        IdGenerator ids;
        const RectangleIds r = AddCornerRectangle(sk, ids, sk.AddPoint(ids, {10, 10}), sk.AddPoint(ids, {40, 30}));
        const std::vector<ObjectId> all(r.sides.begin(), r.sides.end());
        REQUIRE(MoveEntities(sk, ids, all, {{10, 10}, angle, {}}));
        CHECK(CountOf(sk, ConstraintType::Horizontal) == 2);
        CHECK(CountOf(sk, ConstraintType::Vertical) == 2);
        for (const Constraint& c : sk.constraints)
            if (c.type == ConstraintType::Horizontal) CHECK((c.refs[0] == r.sides[1] || c.refs[0] == r.sides[3]));
        REQUIRE(Solve(sk).converged);
        CHECK(SideLength(sk, r.sides[0]) == doctest::Approx(30.0));
        CHECK(SideLength(sk, r.sides[1]) == doctest::Approx(20.0));
        CHECK(std::abs(sk.PositionOf(r.corners[1]).x - 10.0) == doctest::Approx(0.0).epsilon(1e-6));   // turned, not snapped back
    }
}

TEST_CASE("a half turn keeps horizontal and vertical as they are") {
    Sketch sk;
    IdGenerator ids;
    const RectangleIds r = AddCornerRectangle(sk, ids, sk.AddPoint(ids, {0, 0}), sk.AddPoint(ids, {30, 20}));
    const std::vector<ObjectId> all(r.sides.begin(), r.sides.end());
    REQUIRE(MoveEntities(sk, ids, all, {{0, 0}, kPi, {}}));
    for (const Constraint& c : sk.constraints)
        if (c.type == ConstraintType::Horizontal) CHECK((c.refs[0] == r.sides[0] || c.refs[0] == r.sides[2]));
    REQUIRE(Solve(sk).converged);
    CHECK(SideLength(sk, r.sides[0]) == doctest::Approx(30.0));
}

TEST_CASE("a quarter-turn pattern of a rectangle keeps every copy whole") {
    Sketch sk;
    IdGenerator ids;
    const RectangleIds r = AddCornerRectangle(sk, ids, sk.AddPoint(ids, {10, 10}), sk.AddPoint(ids, {20, 15}));
    const std::vector<ObjectId> all(r.sides.begin(), r.sides.end());
    std::vector<std::vector<ObjectId>> copies;
    for (int i = 1; i < 4; ++i) copies.push_back(CopyEntities(sk, ids, all, {{0, 0}, i * kPi / 2, {}}));
    REQUIRE(Solve(sk).converged);
    for (const auto& copy : copies) {
        REQUIRE(copy.size() == 4);
        std::vector<double> sides;
        for (const ObjectId l : copy) sides.push_back(SideLength(sk, l));
        std::sort(sides.begin(), sides.end());
        CHECK(sides[0] == doctest::Approx(5.0));
        CHECK(sides[1] == doctest::Approx(5.0));
        CHECK(sides[2] == doctest::Approx(10.0));
        CHECK(sides[3] == doctest::Approx(10.0));
    }
}

TEST_CASE("turning a line parallel to one left in place") {
    const auto setUp = [](Sketch& sk, IdGenerator& ids, ObjectId& turned) {
        const ObjectId still = sk.AddLine(ids, sk.AddPoint(ids, {0, 0}), sk.AddPoint(ids, {10, 0}));
        turned = sk.AddLine(ids, sk.AddPoint(ids, {0, 5}), sk.AddPoint(ids, {10, 5}));
        Constraint par;
        par.type = ConstraintType::Parallel;
        par.refs = {still, turned, {}};
        sk.AddConstraint(ids, par);
    };
    SUBCASE("a quarter turn: perpendicular") {
        Sketch sk;
        IdGenerator ids;
        ObjectId turned;
        setUp(sk, ids, turned);
        REQUIRE(MoveEntities(sk, ids, std::span(&turned, 1), {{0, 5}, kPi / 2, {}}));
        CHECK(CountOf(sk, ConstraintType::Parallel) == 0);
        CHECK(CountOf(sk, ConstraintType::Perpendicular) == 1);
        REQUIRE(Solve(sk).converged);
        CHECK(sk.PositionOf(sk.FindLine(turned)->end).x == doctest::Approx(0.0).epsilon(1e-6));
    }
    SUBCASE("another angle: dropped") {
        Sketch sk;
        IdGenerator ids;
        ObjectId turned;
        setUp(sk, ids, turned);
        REQUIRE(MoveEntities(sk, ids, std::span(&turned, 1), {{0, 5}, kPi / 6, {}}));
        CHECK(CountOf(sk, ConstraintType::Parallel) == 0);
        CHECK(CountOf(sk, ConstraintType::Perpendicular) == 0);
    }
}

TEST_CASE("a quarter turn swaps horizontal and vertical distances between turned points") {
    Sketch sk;
    IdGenerator ids;
    const ObjectId a = sk.AddPoint(ids, {0, 0}), b = sk.AddPoint(ids, {10, 0});
    const ObjectId line = sk.AddLine(ids, a, b);
    Constraint dx;
    dx.type = ConstraintType::DistanceX;
    dx.refs = {a, b, {}};
    dx.parameter.value = 10;
    sk.AddConstraint(ids, dx);
    REQUIRE(MoveEntities(sk, ids, std::span(&line, 1), {{0, 0}, kPi / 2, {}}));
    CHECK(CountOf(sk, ConstraintType::DistanceX) == 0);
    CHECK(CountOf(sk, ConstraintType::DistanceY) == 1);
}
