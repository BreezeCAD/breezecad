#pragma once

#include <algorithm>
#include <cmath>

namespace bz {

inline constexpr double kPi = 3.14159265358979323846;

/// Plain 2D vector for sketch geometry. Kernel-agnostic on purpose.
struct Vec2 {
    double x = 0.0;
    double y = 0.0;

    constexpr Vec2 operator+(Vec2 o) const { return {x + o.x, y + o.y}; }
    constexpr Vec2 operator-(Vec2 o) const { return {x - o.x, y - o.y}; }
    constexpr Vec2 operator*(double s) const { return {x * s, y * s}; }
    constexpr Vec2 operator/(double s) const { return {x / s, y / s}; }
    constexpr bool operator==(const Vec2&) const = default;

    constexpr double Dot(Vec2 o) const { return x * o.x + y * o.y; }
    constexpr double Cross(Vec2 o) const { return x * o.y - y * o.x; }
    double Length() const { return std::hypot(x, y); }
    Vec2 Normalized() const {
        const double len = Length();
        return len > 0.0 ? *this / len : Vec2{};
    }
    constexpr Vec2 Perp() const { return {-y, x}; }
};

/// Plain 3D vector. Used for planes and camera data outside the kernel.
struct Vec3 {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;

    constexpr Vec3 operator+(Vec3 o) const { return {x + o.x, y + o.y, z + o.z}; }
    constexpr Vec3 operator-(Vec3 o) const { return {x - o.x, y - o.y, z - o.z}; }
    constexpr Vec3 operator*(double s) const { return {x * s, y * s, z * s}; }
    constexpr Vec3 operator-() const { return {-x, -y, -z}; }
    constexpr bool operator==(const Vec3&) const = default;

    constexpr double Dot(Vec3 o) const { return x * o.x + y * o.y + z * o.z; }
    constexpr Vec3 Cross(Vec3 o) const {
        return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x};
    }
    double Length() const { return std::sqrt(Dot(*this)); }
    Vec3 Normalized() const {
        const double len = Length();
        return len > 0.0 ? *this * (1.0 / len) : Vec3{};
    }
};

/// Axis-aligned 2D box. Starts empty; Include() grows it.
struct Box2 {
    Vec2 min{1e300, 1e300};
    Vec2 max{-1e300, -1e300};

    bool IsEmpty() const { return min.x > max.x; }
    void Include(Vec2 p) {
        min = {std::min(min.x, p.x), std::min(min.y, p.y)};
        max = {std::max(max.x, p.x), std::max(max.y, p.y)};
    }
    void Include(Vec2 center, double radius) {
        Include(center - Vec2{radius, radius});
        Include(center + Vec2{radius, radius});
    }
};

/// Axis-aligned 3D box. Starts empty; Include() grows it.
struct Box3 {
    Vec3 min{1e300, 1e300, 1e300};
    Vec3 max{-1e300, -1e300, -1e300};

    bool IsEmpty() const { return min.x > max.x; }
    void Include(Vec3 p) {
        min = {std::min(min.x, p.x), std::min(min.y, p.y), std::min(min.z, p.z)};
        max = {std::max(max.x, p.x), std::max(max.y, p.y), std::max(max.z, p.z)};
    }
    void Include(const Box3& b) {
        if (b.IsEmpty()) return;
        Include(b.min);
        Include(b.max);
    }
};

/// Right-handed orthonormal frame: a sketch plane or a workplane.
/// xDir and normal are unit and perpendicular; yDir = normal x xDir.
struct Frame {
    Vec3 origin{0, 0, 0};
    Vec3 xDir{1, 0, 0};
    Vec3 normal{0, 0, 1};

    Vec3 YDir() const { return normal.Cross(xDir); }
    Vec3 ToWorld(Vec2 p) const { return origin + xDir * p.x + YDir() * p.y; }
    Vec2 ToLocal(Vec3 p) const {
        const Vec3 d = p - origin;
        return {d.Dot(xDir), d.Dot(YDir())};
    }
};

/// Below this a component of a unit direction is noise, not intent: a face
/// picked on a body drifted by 1e-7 gave a sketch normal (-3.6e-12, -1, 0)
/// (stabilization plan, stage 2; «Кубик монолит», 12.11.2026).
inline constexpr double kDirectionNoise = 1e-9;
/// Coordinates are kept on this grid (mm): far finer than anything drawn,
/// coarser than the 1e-10 noise of a measured position.
inline constexpr double kPositionGrid = 1e-9;

/// `d` with its noise components set to zero, unit again.
inline Vec3 CleanDirection(Vec3 d) {
    d = d.Normalized();
    const auto clean = [](double c) { return std::abs(c) < kDirectionNoise ? 0.0 : c; };
    return Vec3{clean(d.x), clean(d.y), clean(d.z)}.Normalized();
}
/// `p` rounded to kPositionGrid.
inline Vec3 CleanPosition(Vec3 p) {
    const auto clean = [](double c) { return std::round(c / kPositionGrid) * kPositionGrid; };
    return {clean(p.x), clean(p.y), clean(p.z)};
}
/// A frame with clean directions (x made square to the normal again) and a
/// clean origin: so a sketch on a face of an axis-aligned part is exactly
/// axis-aligned, and faces made from it meet the part's own faces exactly.
inline Frame CleanFrame(const Frame& f) {
    Frame out;
    out.normal = CleanDirection(f.normal);
    out.xDir = CleanDirection(f.xDir - out.normal * f.xDir.Dot(out.normal));
    out.origin = CleanPosition(f.origin);
    return out;
}

} // namespace bz
