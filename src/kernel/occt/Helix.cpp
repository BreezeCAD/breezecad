#include "kernel/occt/Helix.h"

#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepLib.hxx>
#include <GCE2d_MakeSegment.hxx>
#include <Geom2d_TrimmedCurve.hxx>
#include <Geom_CylindricalSurface.hxx>

#include <algorithm>
#include <atomic>
#include <cmath>

namespace bz::kernel::occt {
namespace {

std::atomic<HelixSplit> gSplit{HelixSplit::Turns};

} // namespace

HelixSplit DefaultHelixSplit() { return gSplit.load(); }
void SetHelixSplitForMeasurement(HelixSplit split) { gSplit.store(split); }

Expected<TopoDS_Wire> MakeHelix(const gp_Ax3& frame, double radius, double turns, double height, bool leftHanded, HelixSplit split) {
    if (radius <= 0 || turns <= 0 || height <= 0) return Error{"Винтовая линия: размеры должны быть больше нуля"};
    Handle(Geom_CylindricalSurface) cylinder = new Geom_CylindricalSurface(frame, radius);
    int pieces = 1;
    switch (split) {
        case HelixSplit::HalfTurns: pieces = std::max(1, static_cast<int>(std::ceil(turns * 2 - 1e-9))); break;
        case HelixSplit::Turns: pieces = std::max(1, static_cast<int>(std::ceil(turns - 1e-9))); break;
        case HelixSplit::Single: pieces = 1; break;
    }
    const double angle = 2 * M_PI * turns * (leftHanded ? -1.0 : 1.0);
    BRepBuilderAPI_MakeWire wire;
    for (int i = 0; i < pieces; ++i) {
        const double t0 = static_cast<double>(i) / pieces, t1 = static_cast<double>(i + 1) / pieces;
        const Handle(Geom2d_TrimmedCurve) segment =
            GCE2d_MakeSegment(gp_Pnt2d(angle * t0, height * t0), gp_Pnt2d(angle * t1, height * t1));
        TopoDS_Edge edge = BRepBuilderAPI_MakeEdge(segment, cylinder);
        // The 3D curve is an approximation: enough spans for the turns one edge holds.
        const int spans = std::max(30, static_cast<int>(std::ceil(turns / pieces * 8)));
        if (!BRepLib::BuildCurve3d(edge, 1e-5, GeomAbs_C1, 14, spans)) return Error{"Не удалось построить винтовую линию"};
        wire.Add(edge);
    }
    if (!wire.IsDone()) return Error{"Не удалось построить винтовую линию"};
    return wire.Wire();
}

} // namespace bz::kernel::occt
