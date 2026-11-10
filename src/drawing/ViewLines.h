#pragma once
// The lines of a model's view before scale and place (ADR-0012, step 3):
// its edges and silhouettes projected on the view plane, split into those
// seen and those behind the material. The kernel computes them
// (kernel/drawing/ViewProjection.h); the sheet, PDF and tests read them.
//
// Model millimetres; x to the right and y up on paper; the origin is where
// the model's origin falls, so a view keeps its place when the part grows.

#include "core/Math.h"

#include <vector>

namespace bz::drawing {

using Polyline = std::vector<Vec2>;

struct ViewLines {
    std::vector<Polyline> visible;   ///< edges and silhouettes seen (ГОСТ 2.303: сплошная основная)
    std::vector<Polyline> hidden;    ///< those behind the material (штриховая)
    Vec2 min, max;                   ///< around both; equal when there are no lines

    bool   Empty() const { return visible.empty() && hidden.empty(); }
    double Width() const { return max.x - min.x; }
    double Height() const { return max.y - min.y; }
};

/// Sum of the lengths of `lines`, mm.
double TotalLength(const std::vector<Polyline>& lines);

/// The part of `line` between the lengths `from` and `to` along it.
Polyline Slice(const Polyline& line, double from, double to);

/// True when the whole segment a–b lies along `lines`: every point of it is
/// within `tolerance` of a line going the same way (a crossing does not count).
bool Covers(const std::vector<Polyline>& lines, Vec2 a, Vec2 b, double tolerance);

/// Projection puts edges on top of each other: the far edge of a box behind
/// the near one, the back half of a circle seen from its side. A line is drawn
/// once, and a seen one wins over a hidden one (a dashed line under a solid
/// one says nothing). Removes from `visible` what repeats an earlier visible
/// line, from `hidden` what lies on a visible or an earlier hidden one — lines
/// that cross or meet at a corner stay whole — drops what is shorter than
/// `tolerance`, and sets `min`/`max`.
void RemoveOverlaps(ViewLines& lines, double tolerance);

} // namespace bz::drawing
