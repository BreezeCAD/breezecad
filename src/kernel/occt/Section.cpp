#include "kernel/occt/Section.h"
#include "kernel/occt/Ops.h"

#include "core/FaultGuard.h"

#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRep_Tool.hxx>
#include <Geom_Plane.hxx>
#include <TopoDS.hxx>
#include <gp_Pln.hxx>
#include <BRepBndLib.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <Bnd_Box.hxx>
#include <Standard_Failure.hxx>
#include <TopExp_Explorer.hxx>
#include <format>
#include <gp_Ax2.hxx>

#include <algorithm>
#include <cmath>

namespace bz::kernel::occt {

Expected<std::optional<TopoDS_Shape>> CutByPlane(const TopoDS_Shape& body, const Frame& plane, bool keepFront) {
    Bnd_Box box;
    BRepBndLib::Add(body, box);
    if (box.IsVoid()) return std::optional<TopoDS_Shape>{};
    double x0, y0, z0, x1, y1, z1;
    box.Get(x0, y0, z0, x1, y1, z1);
    // Big enough to swallow the whole body on its side, wherever the plane is.
    const Vec3 corner{x0, y0, z0}, opposite{x1, y1, z1};
    double reach = (opposite - corner).Length();
    for (const Vec3 c : {corner, opposite}) reach = std::max(reach, (c - plane.origin).Length());
    const double size = 2 * reach + 10.0;

    try {
        const gp_Dir n(plane.normal.x, plane.normal.y, plane.normal.z);
        const gp_Dir x(plane.xDir.x, plane.xDir.y, plane.xDir.z);
        // A block standing on the plane on the side that goes away.
        const double side = keepFront ? -1.0 : 1.0;
        const Vec3 base = plane.origin - plane.xDir * (size / 2) - plane.YDir() * (size / 2) + plane.normal * (side < 0 ? -size : 0.0);
        const TopoDS_Shape cutter = BRepPrimAPI_MakeBox(gp_Ax2(gp_Pnt(base.x, base.y, base.z), n, x), size, size, size).Shape();
        // A section is drawn, not built on: no volume checks, no cleaning.
        ops::Options o;
        o.verify = false;
        o.clean = false;
        const ops::Result cut = ops::Boolean(ops::Kind::Cut, body, cutter, o);
        if (cut.code == ops::Code::Empty) return std::optional<TopoDS_Shape>{};
        if (!cut.Usable()) return Error{"Сечение: не удалось рассечь тело"};
        if (!TopExp_Explorer(cut.shape, TopAbs_SOLID).More()) return std::optional<TopoDS_Shape>{};
        return std::optional<TopoDS_Shape>{cut.shape};
    } catch (const Standard_Failure& failure) {
        return ops::FromFailure(failure, "Ошибка сечения");
    }
}

Expected<std::vector<TopoDS_Face>> CutFaces(const TopoDS_Shape& body, const Frame& plane, double deflection) {
    Bnd_Box box;
    BRepBndLib::Add(body, box);
    if (box.IsVoid()) return std::vector<TopoDS_Face>{};
    double x0, y0, z0, x1, y1, z1;
    box.Get(x0, y0, z0, x1, y1, z1);
    // The plane misses the box: nothing to compute (most bodies of an assembly).
    double lo = 0, hi = 0;
    bool first = true;
    for (const double x : {x0, x1})
        for (const double y : {y0, y1})
            for (const double z : {z0, z1}) {
                const double d = (Vec3{x, y, z} - plane.origin).Dot(plane.normal);
                lo = first ? d : std::min(lo, d);
                hi = first ? d : std::max(hi, d);
                first = false;
            }
    if (lo > 0 || hi < 0) return std::vector<TopoDS_Face>{};
    const Vec3 corner{x0, y0, z0}, opposite{x1, y1, z1};
    double reach = (opposite - corner).Length();
    for (const Vec3 c : {corner, opposite}) reach = std::max(reach, (c - plane.origin).Length());
    reach += 10.0;

    std::vector<TopoDS_Face> faces;
    std::string fault;
    try {
        const bool ok = guard::RunGuarded(
            [&] {
                const gp_Pnt at(plane.origin.x, plane.origin.y, plane.origin.z);
                const gp_Dir n(plane.normal.x, plane.normal.y, plane.normal.z);
                // A frame made from a normal alone may carry any x: take it only across the normal.
                const Vec3 x = plane.xDir - plane.normal * plane.xDir.Dot(plane.normal);
                const gp_Pln pln = x.Length() > 1e-6 ? gp_Pln(gp_Ax3(at, n, gp_Dir(x.x, x.y, x.z))) : gp_Pln(at, n);
                const TopoDS_Face sheet = BRepBuilderAPI_MakeFace(pln, -reach, reach, -reach, reach).Face();
                const ops::Result common = ops::Boolean(ops::Kind::Common, body, sheet, ops::Quick());
                if (!common.Usable() || common.shape.IsNull()) return;
                const TopoDS_Shape cut = common.shape;
                if (!ops::Triangulate(cut, deflection, 0.5, false, 4.0)) return;
                for (TopExp_Explorer f(cut, TopAbs_FACE); f.More(); f.Next()) {
                    TopoDS_Face face = TopoDS::Face(f.Current());
                    TopLoc_Location loc;
                    if (BRep_Tool::Triangulation(face, loc).IsNull()) continue;
                    // Facing the half that is cut away: towards the one who looks in.
                    const Handle(Geom_Plane) surface = Handle(Geom_Plane)::DownCast(BRep_Tool::Surface(face));
                    if (!surface.IsNull()) {
                        gp_Dir n = surface->Pln().Axis().Direction();
                        if (face.Orientation() == TopAbs_REVERSED) n.Reverse();
                        if (n.X() * plane.normal.x + n.Y() * plane.normal.y + n.Z() * plane.normal.z < 0) face.Reverse();
                    }
                    faces.push_back(face);
                }
            },
            &fault);
        if (!ok) return Error{std::format("Сечение: сбой ядра ({})", fault)};
    } catch (const Standard_Failure& failure) {
        return ops::FromFailure(failure, "Ошибка сечения");
    }
    return faces;
}

} // namespace bz::kernel::occt
