#include "render/FloorShadow.h"

#include <cstdint>

namespace bz::render {

int FloorShadowSamples(std::size_t triangles) {
    // About a million triangles cast per light: a few milliseconds for a
    // usual part, some tens for a 100 000-triangle model.
    const std::size_t n = triangles > 0 ? 1'000'000 / triangles : 16;
    return static_cast<int>(std::clamp<std::size_t>(n, 4, 16));
}

namespace {

/// One light: the share of its directions in which each grid point is shaded.
FloorShadowMap CastShadow(std::span<const std::array<Vec3, 3>> triangles, const FloorShadowSpec& spec) {
    FloorShadowMap out;
    const int n = std::max(2, spec.size);
    out.size = n;
    out.value.assign(static_cast<std::size_t>(n) * static_cast<std::size_t>(n), 0.0f);
    if (triangles.empty() || spec.half <= 0 || spec.light.z >= -1e-6) return out;

    // The values stand at the grid's points: point (i, j) is at origin + (i, j) · step.
    const double step = 2.0 * spec.half / (n - 1);
    const Vec2   origin{spec.centre.x - spec.half, spec.centre.y - spec.half};

    // Directions around the light's, evenly over a disc (a sunflower spiral).
    const Vec3 d0 = spec.light.Normalized();
    const Vec3 any = std::abs(d0.x) < 0.9 ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
    const Vec3 u = d0.Cross(any).Normalized(), v = d0.Cross(u);
    const int  samples = std::max(1, spec.samples);

    std::vector<std::uint8_t>  mask(out.value.size());
    std::vector<std::uint16_t> count(out.value.size(), 0);
    for (int k = 0; k < samples; ++k) {
        const double r = samples == 1 ? 0.0 : spec.spread * std::sqrt((k + 0.5) / samples);
        const double a = k * 2.399963229728653;   // the golden angle
        const Vec3   d = (d0 + u * (r * std::cos(a)) + v * (r * std::sin(a))).Normalized();
        if (d.z >= -1e-6) continue;
        // Along d, a point at height h above the floor lands h · shift further.
        const Vec2 shift{d.x / -d.z, d.y / -d.z};
        std::fill(mask.begin(), mask.end(), std::uint8_t{0});
        for (const auto& t : triangles) {
            Vec2 p[3];
            for (int c = 0; c < 3; ++c) {
                const double h = t[c].z - spec.z;
                p[c] = (Vec2{t[c].x, t[c].y} + shift * h - origin) / step;
            }
            const double area = (p[1] - p[0]).Cross(p[2] - p[0]);
            if (std::abs(area) < 1e-12) continue;   // seen edge-on: the faces around it cover
            const int i0 = std::max(0, static_cast<int>(std::ceil(std::min({p[0].x, p[1].x, p[2].x}))));
            const int i1 = std::min(n - 1, static_cast<int>(std::floor(std::max({p[0].x, p[1].x, p[2].x}))));
            const int j0 = std::max(0, static_cast<int>(std::ceil(std::min({p[0].y, p[1].y, p[2].y}))));
            const int j1 = std::min(n - 1, static_cast<int>(std::floor(std::max({p[0].y, p[1].y, p[2].y}))));
            if (i0 > i1 || j0 > j1) continue;
            // Inside when on the inner side of all three edges (a hair of slack:
            // a point on an edge shared by two triangles belongs to both).
            const double sign = area > 0 ? 1.0 : -1.0, eps = -1e-9 * std::abs(area);
            for (int j = j0; j <= j1; ++j)
                for (int i = i0; i <= i1; ++i) {
                    const Vec2 q{static_cast<double>(i), static_cast<double>(j)};
                    const double w0 = sign * (p[1] - p[0]).Cross(q - p[0]);
                    const double w1 = sign * (p[2] - p[1]).Cross(q - p[1]);
                    const double w2 = sign * (p[0] - p[2]).Cross(q - p[2]);
                    if (w0 >= eps && w1 >= eps && w2 >= eps) mask[static_cast<std::size_t>(j) * static_cast<std::size_t>(n) + static_cast<std::size_t>(i)] = 1;
                }
        }
        for (std::size_t c = 0; c < mask.size(); ++c) count[c] = static_cast<std::uint16_t>(count[c] + mask[c]);
    }
    for (std::size_t c = 0; c < count.size(); ++c) out.value[c] = static_cast<float>(count[c]) / static_cast<float>(samples);
    return out;
}

} // namespace

FloorShadowMap ComputeFloorShadow(std::span<const std::array<Vec3, 3>> triangles, const FloorShadowSpec& spec) {
    FloorShadowMap out = CastShadow(triangles, spec);
    const int n = out.size;
    if (spec.sky > 0 && !triangles.empty()) {
        FloorShadowSpec sky = spec;
        sky.light = Vec3{0, 0, -1};
        sky.spread = spec.skySpread;
        sky.samples = std::max(4, spec.samples / 2);
        const FloorShadowMap above = CastShadow(triangles, sky);
        const auto weight = static_cast<float>(spec.sky);
        for (std::size_t c = 0; c < out.value.size(); ++c) out.value[c] = 1 - (1 - out.value[c]) * (1 - weight * above.value[c]);
    }

    // A light blur (3 × 3, twice) smooths the steps between the directions.
    std::vector<float> tmp(out.value.size());
    const auto at = [n](int i, int j) { return static_cast<std::size_t>(j) * static_cast<std::size_t>(n) + static_cast<std::size_t>(i); };
    for (int pass = 0; pass < 2; ++pass) {
        for (int j = 0; j < n; ++j)
            for (int i = 0; i < n; ++i)
                tmp[at(i, j)] = (out.value[at(std::max(i - 1, 0), j)] + out.value[at(i, j)] + out.value[at(std::min(i + 1, n - 1), j)]) / 3.0f;
        for (int j = 0; j < n; ++j)
            for (int i = 0; i < n; ++i)
                out.value[at(i, j)] = (tmp[at(i, std::max(j - 1, 0))] + tmp[at(i, j)] + tmp[at(i, std::min(j + 1, n - 1))]) / 3.0f;
    }
    return out;
}

} // namespace bz::render
