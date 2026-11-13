#include "render/FloorShadow.h"

#include <doctest.h>

#include <vector>

using namespace bz;
using namespace bz::render;

namespace {

/// The twelve triangles of a box.
std::vector<std::array<Vec3, 3>> Box(Vec3 lo, Vec3 hi) {
    const auto p = [&](int i) { return Vec3{i & 1 ? hi.x : lo.x, i & 2 ? hi.y : lo.y, i & 4 ? hi.z : lo.z}; };
    static constexpr int kQuads[6][4] = {{0, 1, 3, 2}, {4, 5, 7, 6}, {0, 1, 5, 4}, {2, 3, 7, 6}, {0, 2, 6, 4}, {1, 3, 7, 5}};
    std::vector<std::array<Vec3, 3>> out;
    for (const auto& q : kQuads) {
        out.push_back({p(q[0]), p(q[1]), p(q[2])});
        out.push_back({p(q[0]), p(q[2]), p(q[3])});
    }
    return out;
}

/// The shadow at a point of the floor (the nearest grid point).
float ShadowAt(const FloorShadowMap& map, const FloorShadowSpec& spec, double x, double y) {
    const double step = 2.0 * spec.half / (map.size - 1);
    const int i = static_cast<int>(std::lround((x - spec.centre.x + spec.half) / step));
    const int j = static_cast<int>(std::lround((y - spec.centre.y + spec.half) / step));
    return map.At(i, j);
}

} // namespace

TEST_CASE("Floor shadow: a box from straight above shades its own footprint") {
    const auto box = Box({-10, -10, 0}, {10, 10, 10});
    FloorShadowSpec spec;
    spec.half = 40;
    spec.spread = 0;
    spec.samples = 1;
    const FloorShadowMap map = ComputeFloorShadow(box, spec);
    CHECK(ShadowAt(map, spec, 0, 0) == doctest::Approx(1.0));
    CHECK(ShadowAt(map, spec, 5, -5) == doctest::Approx(1.0));
    CHECK(ShadowAt(map, spec, 25, 0) == doctest::Approx(0.0));
    CHECK(ShadowAt(map, spec, 0, -30) == doctest::Approx(0.0));
}

TEST_CASE("Floor shadow: a slanted light shifts it, by the height") {
    const auto box = Box({-5, -5, 20}, {5, 5, 21});   // a plate hanging 20 mm up
    FloorShadowSpec spec;
    spec.half = 60;
    spec.spread = 0;
    spec.samples = 1;
    spec.light = Vec3{1, 0, -1};   // 45°: a 20 mm height lands 20 mm aside
    const FloorShadowMap map = ComputeFloorShadow(box, spec);
    CHECK(ShadowAt(map, spec, 0, 0) == doctest::Approx(0.0));
    CHECK(ShadowAt(map, spec, 20.5, 0) == doctest::Approx(1.0));
}

TEST_CASE("Floor shadow: the half-shadow is narrow at the foot and wide under what is high") {
    FloorShadowSpec spec;
    spec.half = 80;
    spec.spread = 0.25;
    spec.samples = 16;
    // The rim of a plate standing on the floor and of one 40 mm up: across
    // the rim (x from -20 to 20 mm around its edge at x = 10), how many grid
    // points are in a half-shadow (neither lit nor full).
    const auto halfShadowWidth = [&](double height) {
        const FloorShadowMap map = ComputeFloorShadow(Box({-10, -10, height}, {10, 10, height + 1}), spec);
        int partial = 0;
        for (double x = -10; x <= 30; x += 0.5) {
            const float s = ShadowAt(map, spec, x, 0);
            partial += s > 0.05f && s < 0.95f ? 1 : 0;
        }
        return partial;
    };
    CHECK(halfShadowWidth(40) > 2 * halfShadowWidth(0));
    // Full shadow right under the low plate, lit far away.
    const FloorShadowMap low = ComputeFloorShadow(Box({-10, -10, 0}, {10, 10, 1}), spec);
    CHECK(ShadowAt(low, spec, 0, 0) == doctest::Approx(1.0).epsilon(0.02));
    CHECK(ShadowAt(low, spec, 60, 60) == doctest::Approx(0.0));
}

TEST_CASE("Floor shadow: fewer directions for a bigger model") {
    CHECK(FloorShadowSamples(1000) == 16);
    CHECK(FloorShadowSamples(10'000'000) == 4);
    CHECK(FloorShadowSamples(250'000) < 16);
}

TEST_CASE("Floor shadow: the sky shades around the foot, fading away") {
    FloorShadowSpec spec;
    spec.half = 80;
    spec.spread = 0;
    spec.samples = 1;
    spec.light = Vec3{1, 0, -1};
    const auto box = Box({-10, -10, 0}, {10, 10, 20});
    const FloorShadowMap lampOnly = ComputeFloorShadow(box, spec);
    spec.sky = 0.5;
    const FloorShadowMap withSky = ComputeFloorShadow(box, spec);
    // Before the box (away from the lamp's shadow): only the sky shades.
    CHECK(ShadowAt(lampOnly, spec, -13, 0) == doctest::Approx(0.0));
    CHECK(ShadowAt(withSky, spec, -13, 0) > 0.05f);
    CHECK(ShadowAt(withSky, spec, -13, 0) > ShadowAt(withSky, spec, -40, 0));
    CHECK(ShadowAt(withSky, spec, -13, 0) <= 0.5f);
}

TEST_CASE("Floor shadow: a see-through body casts a lighter one") {
    FloorShadowSpec spec;
    spec.half = 40;
    spec.spread = 0;
    spec.samples = 1;
    const auto box = Box({-10, -10, 0}, {10, 10, 10});
    const std::vector<float> clear(box.size(), 0.3f);
    const FloorShadowMap map = ComputeFloorShadow(box, spec, clear);
    CHECK(ShadowAt(map, spec, 0, 0) == doctest::Approx(0.3).epsilon(0.02));
}
