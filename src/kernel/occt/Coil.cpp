#include "kernel/occt/Coil.h"
#include "kernel/occt/Helix.h"
#include "kernel/occt/Ops.h"
#include "kernel/occt/ToolCache.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <BRepLib.hxx>
#include <BRepOffsetAPI_MakePipeShell.hxx>
#include <BRepTools.hxx>
#include <GCE2d_MakeSegment.hxx>
#include <GProp_GProps.hxx>
#include <Geom_CylindricalSurface.hxx>
#include <Standard_Failure.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <format>
#include <gp_Ax3.hxx>

#include <algorithm>
#include <sstream>
#include <string>
#include <cmath>

namespace bz::kernel::occt {
namespace {

/// A solid from sweeping `profile` along `helix` with a fixed axis direction.
Expected<TopoDS_Shape> SweepWire(const TopoDS_Wire& profile, const TopoDS_Wire& helix, const gp_Dir& axis) {
    BRepOffsetAPI_MakePipeShell sweep(helix);
    sweep.SetMode(axis);   // the section keeps its tilt to the axis, as a coil does
    sweep.Add(profile, false, false);
    Handle(Deadline) deadline = new Deadline(ops::kSweepSeconds);
    sweep.Build(Message_ProgressRange(deadline->Start()));
    if (deadline->Passed()) return Error{"Пружина не уложилась в отведённое время — уменьшите число витков или упростите профиль"};
    if (!sweep.IsDone() || !sweep.MakeSolid()) return Error{"Пружина не строится с такими размерами"};
    return sweep.Shape();
}

} // namespace

namespace {

/// The coils swept last (ToolCache.h).
ToolCache& Cache() {
    static ToolCache cache(8);
    return cache;
}

/// Everything a coil's tool depends on: the profile as it is in space, the
/// axis, the sizes, the way the helix is split.
std::string KeyOf(const std::vector<TopoDS_Face>& faces, const doc::CoilFeature& f, const Vec3& a, const Vec3& b) {
    std::ostringstream profile;
    for (const TopoDS_Face& face : faces) BRepTools::Write(face, profile);
    std::string key = profile.str();
    for (double v : {a.x, a.y, a.z, b.x, b.y, b.z, f.pitch.value, f.turns.value}) AddToKey(key, v);
    key += f.leftHanded ? "L" : "R";
    key += std::to_string(static_cast<int>(DefaultHelixSplit()));
    return key;
}

} // namespace

Expected<TopoDS_Shape> MakeCoil(const std::vector<TopoDS_Face>& faces, const doc::CoilFeature& f, const sketch::Sketch& sk,
                                const Frame& frame, CoilNotes* notes) {
    if (notes) *notes = {};
    for (const Parameter* p : {&f.pitch, &f.turns})
        if (p->HasError()) return Error{p->error};
    if (f.pitch.value <= 1e-6 || f.turns.value <= 1e-6) return Error{"Шаг и число витков должны быть больше нуля"};
    if (f.turns.value > 500) return Error{"Слишком много витков (больше 500)"};
    const sketch::Line* line = sk.FindLine(f.axisLine);
    if (!f.axisLine) return Error{"Нарисуйте в эскизе осевую линию и щёлкните её — вокруг неё навивается пружина"};
    if (!line) return Error{"Ось пружины удалена из эскиза"};
    if (faces.empty()) return Error{"В эскизе нет замкнутого контура для профиля"};

    const Vec3 a = frame.ToWorld(sk.PositionOf(line->start)), b = frame.ToWorld(sk.PositionOf(line->end));
    if ((b - a).Length() < 1e-9) return Error{"Ось пружины нулевой длины"};
    const Vec3 dir = (b - a).Normalized();

    try {
        // Where the profile sits relative to the axis: its centre gives the
        // helix radius and starting height; its extent along the axis must
        // stay below the pitch or the turns run into each other.
        GProp_GProps props;
        for (const TopoDS_Face& face : faces) BRepGProp::SurfaceProperties(face, props);
        const gp_Pnt c = props.CentreOfMass();
        const Vec3 centre{c.X(), c.Y(), c.Z()};
        const double along = (centre - a).Dot(dir);
        const Vec3 foot = a + dir * along;
        const double radius = (centre - foot).Length();
        if (radius < 1e-6) return Error{"Профиль лежит на оси — отодвиньте его от осевой линии"};
        double lo = 1e300, hi = -1e300, nearest = 1e300;
        for (const TopoDS_Face& face : faces)
            for (TopExp_Explorer e(face, TopAbs_EDGE); e.More(); e.Next()) {
                const BRepAdaptor_Curve curve(TopoDS::Edge(e.Current()));
                for (int i = 0; i <= 24; ++i) {
                    const gp_Pnt p = curve.Value(curve.FirstParameter() + (curve.LastParameter() - curve.FirstParameter()) * i / 24.0);
                    const Vec3 v = Vec3{p.X(), p.Y(), p.Z()} - a;
                    const double h = v.Dot(dir);
                    lo = std::min(lo, h);
                    hi = std::max(hi, h);
                    nearest = std::min(nearest, (v - dir * h).Length());
                }
            }
        if (nearest < 1e-6) return Error{"Профиль касается оси или пересекает её — отодвиньте его от осевой линии"};
        if (hi - lo >= f.pitch.value - 1e-6) return Error{"Шаг меньше высоты профиля: витки пересекаются"};
        // What is left between two turns along the axis: a coil's gap, or
        // the web between two turns of a groove.
        if (notes) notes->web = f.pitch.value - (hi - lo);
        const std::string key = KeyOf(faces, f, a, b);
        if (std::optional<TopoDS_Shape> cached = Cache().Find(key)) {
            if (notes) notes->cached = true;
            return *cached;
        }

        // The helix on the cylinder through the profile centre (its edges: Helix.h).
        const gp_Dir axis(dir.x, dir.y, dir.z);
        const gp_Dir across((centre - foot).x, (centre - foot).y, (centre - foot).z);
        const gp_Ax3 helixFrame(gp_Pnt(foot.x, foot.y, foot.z), axis, across);
        const Expected<TopoDS_Wire> helixWire =
            MakeHelix(helixFrame, radius, f.turns.value, f.pitch.value * f.turns.value, f.leftHanded);
        if (!helixWire) return Error{"Пружина: не удалось построить винтовую линию"};
        const TopoDS_Wire helix = *helixWire;

        // Each region: its outer boundary swept, its holes swept and cut out.
        TopoDS_Shape result;
        for (const TopoDS_Face& face : faces) {
            const TopoDS_Wire outer = BRepTools::OuterWire(face);
            Expected<TopoDS_Shape> solid = SweepWire(outer, helix, axis);
            if (!solid) return solid;
            TopoDS_Shape piece = *solid;
            for (TopExp_Explorer w(face, TopAbs_WIRE); w.More(); w.Next()) {
                const TopoDS_Wire wire = TopoDS::Wire(w.Current());
                if (wire.IsSame(outer)) continue;
                Expected<TopoDS_Shape> hole = SweepWire(wire, helix, axis);
                if (!hole) return hole;
                piece = ops::OrThrow(ops::Cut(piece, *hole, ops::Tool()));
            }
            result = result.IsNull() ? piece : ops::OrThrow(ops::Fuse(result, piece, ops::Tool()));
        }
        if (!BRepCheck_Analyzer(result).IsValid()) return Error{"Пружина получилась некорректной"};
        Cache().Put(key, result);
        return result;
    } catch (const ops::Failed& failed) {
        return Error{failed.what()};
    } catch (const Standard_Failure& failure) {
        return ops::FromFailure(failure, "Ошибка пружины");
    }
}

} // namespace bz::kernel::occt
