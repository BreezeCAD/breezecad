#pragma once
// «Живой вид»: the soft shadow on the floor, computed on the processor.
//
// OCCT's shadow map (8.0.1) filters over one texel only: the edge stays hard
// at any resolution, and a coarser map only breaks it into steps and specks
// on the faces (checked at 512 and 1024). So the floor takes its shadow from
// here instead: the bodies' triangles are cast onto the floor along a few
// directions spread around the light's, as from a lamp of some size. Where
// all of them fall the shadow is full; at its rim only some do — a half-
// shadow that widens with the height of what casts it and is sharp where the
// body stands, as a real one.
//
// Two lights: the lamp (slanted, small — the shadow aside) and the sky
// (from above, wide — a faint shade around the foot).
//
// Counted once per change of the bodies, not per frame: the light stands
// still over the floor (not with the camera), so turning the view costs
// nothing.

#include "core/Math.h"

#include <array>
#include <span>
#include <vector>

namespace bz::render {

struct FloorShadowSpec {
    Vec2   centre;              ///< of the floor square, mm
    double half = 1;            ///< half its side, mm
    double z = 0;               ///< the floor's height
    Vec3   light{0, 0, -1};     ///< the way the light goes (down: z < 0)
    double spread = 0.15;       ///< the lamp's angular radius, radians
    int    size = 160;          ///< cells a side
    int    samples = 16;        ///< directions the triangles are cast along
    /// The light of the sky: from straight above, wide (skySpread), adds a
    /// faint shade all around the foot — the body "stands". Weight 0 — none.
    double sky = 0;
    double skySpread = 1.0;
};

struct FloorShadowMap {
    int                size = 0;
    std::vector<float> value;   ///< size × size, row by row (y), 0 — lit, 1 — full shadow
    float At(int i, int j) const { return value[static_cast<std::size_t>(j) * static_cast<std::size_t>(size) + static_cast<std::size_t>(i)]; }
};

/// How many directions to cast along for so many triangles: fewer for a big
/// model, so a change stays quick (the shadow only gets coarser).
int FloorShadowSamples(std::size_t triangles);

/// `opacity` (empty: all opaque) — one per triangle, 0..1: a see-through
/// body (clear polycarbonate) casts a lighter shadow.
FloorShadowMap ComputeFloorShadow(std::span<const std::array<Vec3, 3>> triangles, const FloorShadowSpec& spec,
                                  std::span<const float> opacity = {});

} // namespace bz::render
