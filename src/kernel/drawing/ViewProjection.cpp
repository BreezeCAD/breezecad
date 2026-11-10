#include "kernel/drawing/ViewProjection.h"

#include "kernel/occt/ModelImpl.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepLib.hxx>
#include <BRep_Builder.hxx>
#include <Bnd_Box.hxx>
#include <GCPnts_TangentialDeflection.hxx>
#include <HLRAlgo_Projector.hxx>
#include <HLRBRep_Algo.hxx>
#include <HLRBRep_HLRToShape.hxx>
#include <Standard_Failure.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <gp_Ax2.hxx>

#include <algorithm>
#include <cmath>
#include <format>

namespace bz::kernel {

namespace {

// Faces that meet at less than this are one smooth surface to the eye: their
// common edge is a tangent edge, not a sharp one. Fillets and extrusions mark
// such edges themselves; this is for the edges nobody marked (an imported
// body, the result of a cut). OCCT's own default angle (1e-10) is too strict
// for bodies built with ordinary tolerances.
constexpr double kSmoothAngle = 0.5 * kPi / 180.0;   // rad
// Curves become polylines: no farther than this from the curve, and no
// turn sharper than kTurn between neighbours (small arcs stay round).
constexpr double kTurn = 0.1;   // rad

/// How fine the polylines are for a model of size `diagonal`, model mm.
double DeflectionFor(double diagonal) { return std::clamp(diagonal * 1e-4, 1e-4, 0.01); }

/// The edges of an HLR result as polylines. The result lies in the view
/// plane: x right, y up, z = 0.
void Collect(const TopoDS_Shape& edges, double deflection, std::vector<drawing::Polyline>& out) {
    if (edges.IsNull()) return;
    BRepLib::BuildCurves3d(edges);   // HLR gives curves on the view plane only
    for (TopExp_Explorer it(edges, TopAbs_EDGE); it.More(); it.Next()) {
        const BRepAdaptor_Curve curve(TopoDS::Edge(it.Current()));
        GCPnts_TangentialDeflection points(curve, kTurn, deflection);
        if (points.NbPoints() < 2) continue;
        drawing::Polyline line;
        line.reserve(static_cast<std::size_t>(points.NbPoints()));
        for (int i = 1; i <= points.NbPoints(); ++i) {
            const gp_Pnt p = points.Value(i);
            line.push_back({p.X(), p.Y()});
        }
        out.push_back(std::move(line));
    }
}

} // namespace

ViewAxes AxesOf(drawing::ViewProjection projection) {
    switch (projection) {
        case drawing::ViewProjection::Front: return {{1, 0, 0}, {0, 0, 1}};
        case drawing::ViewProjection::Top:   return {{1, 0, 0}, {0, 1, 0}};
        case drawing::ViewProjection::Left:  return {{0, -1, 0}, {0, 0, 1}};
    }
    return {};
}

Expected<drawing::ViewLines> ProjectView(const Model& model, const ViewAxes& axes) {
    drawing::ViewLines lines;
    const std::vector<Body>& bodies = model.GetImpl().bodies;
    if (bodies.empty()) return lines;
    try {
        BRep_Builder builder;
        TopoDS_Compound all;
        builder.MakeCompound(all);
        for (const Body& body : bodies)
            if (!body.shape.IsNull()) builder.Add(all, body.shape);
        Bnd_Box box;
        BRepBndLib::Add(all, box);
        if (box.IsVoid()) return lines;
        const double deflection = DeflectionFor(std::sqrt(box.SquareExtent()));

        // A copy of the topology (the geometry is shared): marking smooth edges
        // writes into the shape, and the model's own bodies must stay as they are.
        const TopoDS_Shape shape = BRepBuilderAPI_Copy(all, Standard_False).Shape();
        BRepLib::EncodeRegularity(shape, kSmoothAngle);

        const Vec3 toViewer = axes.right.Cross(axes.up);
        const gp_Ax2 plane(gp_Pnt(0, 0, 0), gp_Dir(toViewer.x, toViewer.y, toViewer.z), gp_Dir(axes.right.x, axes.right.y, axes.right.z));
        Handle(HLRBRep_Algo) hlr = new HLRBRep_Algo();
        hlr->Add(shape);
        hlr->Projector(HLRAlgo_Projector(plane));   // parallel projection on the plane's XY
        hlr->Update();
        hlr->Hide();

        // Sharp edges and outlines (silhouettes). Left out on purpose: Rg1Line (smooth edges),
        // RgNLine (seams and edges inside one smooth surface), IsoLine.
        HLRBRep_HLRToShape result(hlr);
        Collect(result.VCompound(), deflection, lines.visible);
        Collect(result.OutLineVCompound(), deflection, lines.visible);
        Collect(result.HCompound(), deflection, lines.hidden);
        Collect(result.OutLineHCompound(), deflection, lines.hidden);
        drawing::RemoveOverlaps(lines, 3 * deflection);
        return lines;
    } catch (const Standard_Failure& failure) {
        return Error{std::format("Вид чертежа: не удалось построить проекцию ({})", failure.GetMessageString())};
    }
}

} // namespace bz::kernel
