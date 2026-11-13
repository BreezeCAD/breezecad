#pragma once
// A model edge as it appears on a sketch plane ("Проецировать геометрию").
// OCCT-free: the sketch editor makes sketch curves from it.

#include "core/Id.h"
#include "core/Math.h"
#include "sketch/Sketch.h"

#include <optional>
#include <vector>

namespace bz::kernel {

struct ProjectedCurve {
    enum class Kind : unsigned char { Line, Circle, Arc, Ellipse, Spline } kind = Kind::Line;
    Vec2   start, end;      ///< line ends; arc ends, counter-clockwise on the sketch plane;
                            ///< ellipse: the ends of its major and minor axes; spline: its ends
    Vec2   centre;          ///< circles, arcs and ellipses
    double radius = 0.0;
    /// Spline: the curve through these points, in the edge's direction
    /// (kSplineSpans + 1 of them, more for a winding curve, up to 4 times as
    /// many; closed: the last is the first). Any edge
    /// that is not a line, circle or arc on the plane projects so — an
    /// ellipse, a slanted arc, a spline of a loft or sweep — as in Inventor.
    std::vector<Vec2> points;
};

/// Spans of a projected spline (gentle curve). When the model changes the
/// spline keeps its points and only moves them — resampled to its own count
/// if the edge now asks for another — so what is tied to its ends follows.
inline constexpr int kSplineSpans = 16;

/// A body of revolution as it appears on a sketch plane: its axis, its
/// outline (the extreme positions of the profile) and where the plane cuts it.
struct RevolvedProjection {
    std::optional<ProjectedCurve> axis;     ///< a line, when the axis is not seen end-on
    std::optional<Vec2>           centre;   ///< the axis seen end-on
    std::vector<ProjectedCurve>   outline;
    std::vector<ProjectedCurve>   section;  ///< empty when the plane does not cut the body
    bool                          outlineSkipped = false;   ///< axis slanted to the plane: no outline
};

/// Moves the sketch curve `curve` (and its points) to `projected`. False when
/// the sketch has no such curve of that kind.
bool PlaceProjectedCurve(sketch::Sketch& sk, ObjectId curve, const ProjectedCurve& projected);
/// Moves several projected curves of one sketch at once. A line may come the
/// other way round than it was drawn: each takes the way that agrees with
/// the curves sharing its ends. Per entry: placed (false as for PlaceProjectedCurve).
std::vector<bool> PlaceProjectedCurves(sketch::Sketch& sk, const std::vector<std::pair<ObjectId, ProjectedCurve>>& curves);
/// The sketch curve `curve` is where `projected` says (a line either way
/// round; within `tolerance`, mm).
/// A face's outline on the sketch plane whose loops cross or overlap there:
/// split where they cross, a stretch covered twice kept once (see
/// occt::TrimOverlaps). `changed` false when it is left as it was.
std::vector<ProjectedCurve> TrimOverlappingCurves(const std::vector<ProjectedCurve>& curves, bool& changed);
bool CurveMatches(const sketch::Sketch& sk, ObjectId curve, const ProjectedCurve& projected, double tolerance);

/// Puts a projected spline into a sketch: its ends from `endPoint` (shared
/// with the neighbours), the points between from `innerPoint` (new ones).
/// Invalid id when there is nothing to draw.
template <class EndPoint, class InnerPoint>
ObjectId AddProjectedSpline(sketch::Sketch& sk, IdGenerator& ids, const ProjectedCurve& pc, EndPoint&& endPoint, InnerPoint&& innerPoint) {
    if (pc.kind != ProjectedCurve::Kind::Spline || pc.points.size() < 3) return {};
    const bool closed = (pc.points.back() - pc.points.front()).Length() < 1e-9;
    std::vector<ObjectId> points;
    points.push_back(endPoint(pc.points.front()));
    for (std::size_t i = 1; i + 1 < pc.points.size(); ++i) points.push_back(innerPoint(pc.points[i]));
    points.push_back(closed ? points.front() : endPoint(pc.points.back()));
    return sk.AddSpline(ids, std::move(points), true);
}
/// The same with new projected points between the ends.
template <class EndPoint>
ObjectId AddProjectedSpline(sketch::Sketch& sk, IdGenerator& ids, const ProjectedCurve& pc, EndPoint&& endPoint) {
    return AddProjectedSpline(sk, ids, pc, std::forward<EndPoint>(endPoint), [&](Vec2 at) {
        const ObjectId p = sk.AddPoint(ids, at);
        sk.FindPoint(p)->projected = true;
        return p;
    });
}

} // namespace bz::kernel
