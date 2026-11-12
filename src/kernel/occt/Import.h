#pragma once
// OCCT side of kernel/Import.h: turning an imported feature's data block
// back into shapes, and what a mesh body needs that OCCT does not do for a
// face made of triangles only (volume, distance to a point).
//
// A mesh body (STL) is one TopoDS_Face that has a Poly_Triangulation and no
// surface: OCCT shows and picks it like any face, but booleans, fillets,
// BRepGProp and BRepExtrema do not work on it — the model leaves mesh bodies
// out of those and asks the functions below instead.

#include "core/Expected.h"
#include "core/Math.h"
#include "document/ImportFeature.h"
#include "kernel/Projection.h"

#include <Bnd_Box.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Pnt.hxx>

#include <vector>

namespace bz::kernel::occt {

/// The bodies kept in `feature` (solids, open shells or one mesh face), in
/// the order of the file.
Expected<std::vector<TopoDS_Shape>> DecodeImported(const doc::ImportFeature& feature);

/// A face (or a shape made only of faces) with triangles and no surface.
bool IsMesh(const TopoDS_Shape& shape);

struct MeshProperties {
    double  volume = 0.0;   ///< mm³, of the closed triangle set (signed sum: open meshes give an estimate)
    double  area = 0.0;     ///< mm²
    gp_Pnt  centre;         ///< centre of volume (of area when the volume is ~0)
    Bnd_Box box;
    std::size_t triangles = 0;
};
MeshProperties MeasureMesh(const TopoDS_Shape& mesh);

/// Distance from `point` to the nearest triangle of `mesh`.
double MeshDistance(const TopoDS_Shape& mesh, const gp_Pnt& point);

/// Where `plane` cuts `mesh`, on the plane, as lines: the cut through each
/// triangle chained into polylines and thinned (Douglas–Peucker) to what the
/// eye can tell apart, so a sketch gets dozens of lines, not thousands.
std::vector<ProjectedCurve> SectionMesh(const TopoDS_Shape& mesh, const Frame& plane);

} // namespace bz::kernel::occt
