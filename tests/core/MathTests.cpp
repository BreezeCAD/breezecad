// SPDX-License-Identifier: MPL-2.0
#include "core/Math.h"

#include <doctest.h>

using namespace bz;

// A sketch on a picked face kept the noise of the moment for ever: normal
// (-3.6e-12, -1, 0), origin 6.120000000279669 («Кубик монолит», 12.11.2026).
TEST_CASE("a frame picked on a square face is cleaned to the axes and to the grid") {
    Frame picked;
    picked.normal = {-3.582802844213112e-12, -1.0, 0.0};
    picked.xDir = {1.0, -3.582802844213112e-12, 0.0};
    picked.origin = {2.1926753407586243e-11, 6.120000000279669, 0.0};
    const Frame f = CleanFrame(picked);
    CHECK(f.normal.x == 0.0);
    CHECK(f.normal.y == -1.0);
    CHECK(f.xDir.x == 1.0);
    CHECK(f.xDir.y == 0.0);
    CHECK(f.origin.x == 0.0);
    CHECK(f.origin.y == doctest::Approx(6.12).epsilon(1e-15));
    CHECK(std::abs(f.origin.y - 6.12) < 1e-12);
}

TEST_CASE("a frame on a slanted face keeps its direction") {
    Frame slanted;
    slanted.normal = Vec3{0.0, std::sqrt(0.5), std::sqrt(0.5)};
    slanted.xDir = {1.0, 0.0, 0.0};
    slanted.origin = {1.5, 2.25, -3.125};
    const Frame f = CleanFrame(slanted);
    CHECK((f.normal - slanted.normal).Length() < 1e-15);
    CHECK((f.origin - slanted.origin).Length() < 1e-12);
    CHECK(std::abs(f.xDir.Dot(f.normal)) < 1e-15);
}
