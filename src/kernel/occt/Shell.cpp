#include "kernel/occt/Shell.h"

#include "kernel/occt/Ops.h"

#include <BRepCheck_Analyzer.hxx>
#include <BRepOffset_Analyse.hxx>
#include <BRepOffset_MakeOffset.hxx>
#include <BRep_Builder.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS_Shell.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <BRepGProp_Face.hxx>
#include <BRep_Tool.hxx>
#include <Geom2d_Curve.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt2d.hxx>
#include <BRepOffsetAPI_MakeOffsetShape.hxx>
#include <BRepOffsetAPI_MakeThickSolid.hxx>
#include <BRepOffset_Mode.hxx>
#include <GeomAbs_JoinType.hxx>
#include <Standard_Failure.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>

#include <algorithm>
#include <cmath>
#include <optional>

namespace bz::kernel::occt {
namespace {

constexpr double kTolerance = 1e-4;
constexpr const char* kFailed = "Не удалось построить оболочку: толщина слишком большая для этого тела?";
constexpr const char* kTooLong = "Оболочка не уложилась в отведённое время и остановлена — упростите тело или уменьшите толщину";
constexpr const char* kMixedEdges =
    "Оболочка не строится: у тела есть рёбра, вдоль которых грани переходят от угла к касанию "
    "(как у лофта квадрат→круг) — ядро OCCT такие рёбра не смещает. Обход: два лофта, "
    "внешний и внутренний (сечения меньше на толщину), и вырез внутреннего";

/// Angle between the normals of the two faces of `edge` at its parameter `t`.
double DihedralAt(const TopoDS_Edge& edge, const TopoDS_Face& a, const TopoDS_Face& b, double t) {
    const auto normal = [&](const TopoDS_Face& face) -> std::optional<gp_Dir> {
        double first = 0, last = 0;
        const Handle(Geom2d_Curve) uv = BRep_Tool::CurveOnSurface(edge, face, first, last);
        if (uv.IsNull()) return std::nullopt;
        const gp_Pnt2d p = uv->Value(t);
        BRepGProp_Face props(face);
        gp_Pnt at;
        gp_Vec n;
        props.Normal(p.X(), p.Y(), at, n);
        if (n.Magnitude() < 1e-12) return std::nullopt;
        return gp_Dir(n);
    };
    const auto na = normal(a), nb = normal(b);
    return na && nb ? na->Angle(*nb) : 0.0;
}

/// Whether the body has an edge whose faces meet at an angle at one place
/// and smoothly (tangent) at another: the inner offset of such an edge
/// needs a varying transition the OCCT offset cannot make.
bool HasMixedEdges(const TopoDS_Shape& body) {
    TopTools_IndexedDataMapOfShapeListOfShape facesOf;
    TopExp::MapShapesAndAncestors(body, TopAbs_EDGE, TopAbs_FACE, facesOf);
    constexpr double kSmooth = 1.0 * 3.14159265358979 / 180, kSharp = 10.0 * 3.14159265358979 / 180;
    for (int i = 1; i <= facesOf.Extent(); ++i) {
        const TopoDS_Edge& edge = TopoDS::Edge(facesOf.FindKey(i));
        const TopTools_ListOfShape& faces = facesOf.FindFromIndex(i);
        if (faces.Extent() != 2 || BRep_Tool::Degenerated(edge)) continue;
        const TopoDS_Face& a = TopoDS::Face(faces.First());
        const TopoDS_Face& b = TopoDS::Face(faces.Last());
        double first = 0, last = 0;
        if (BRep_Tool::Curve(edge, first, last).IsNull()) continue;
        double smallest = 10, largest = 0;
        for (int k = 0; k <= 8; ++k) {
            const double angle = DihedralAt(edge, a, b, first + (last - first) * k / 8);
            smallest = std::min(smallest, angle);
            largest = std::max(largest, angle);
        }
        if (smallest < kSmooth && largest > kSharp) return true;
    }
    return false;
}

/// The whole body offset by `distance` (negative: inside) with sharp corners.
Expected<TopoDS_Shape> Offset(const TopoDS_Shape& body, double distance) {
    BRepOffsetAPI_MakeOffsetShape offset;
    Handle(Deadline) deadline = new Deadline(ops::kBooleanSeconds);
    offset.PerformByJoin(body, distance, kTolerance, BRepOffset_Skin, false, false, GeomAbs_Intersection, false,
                         Message_ProgressRange(deadline->Start()));
    if (deadline->Passed()) return Error{kTooLong};
    if (!offset.IsDone() || offset.Shape().IsNull()) return Error{kFailed};
    return offset.Shape();
}

double VolumeOf(const TopoDS_Shape& shape) {
    GProp_GProps props;
    BRepGProp::VolumeProperties(shape, props);
    return props.Mass();
}

/// Whether `result` is really a shell of `body`. OCCT 8 reports success for a
/// square-to-circle loft and hands back the body untouched (same volume, no
/// cavity), which passes BRepCheck. A real shell always changes the volume,
/// and an inward one always takes material away.
bool IsHollowed(const TopoDS_Shape& body, const TopoDS_Shape& result, bool outward) {
    const double before = VolumeOf(body), after = VolumeOf(result);
    if (!(after > 0.0)) return false;
    if (std::abs(after - before) <= 1e-4 * std::abs(before)) return false;   // nothing was hollowed
    return outward || after < before;
}

/// The solid between `faces` and their offset by `distance` (OCCT
/// thickening): a slab under one face, or the sharp corner under a group.
std::optional<TopoDS_Shape> Thicken(const TopoDS_Shape& faces, double distance) {
    BRepOffset_MakeOffset offset;
    offset.Initialize(faces, distance, kTolerance, BRepOffset_Skin, false, false, GeomAbs_Intersection, true);
    Handle(Deadline) deadline = new Deadline(ops::kBooleanSeconds);
    offset.MakeOffsetShape(Message_ProgressRange(deadline->Start()));
    if (deadline->Passed() || !offset.IsDone() || offset.Shape().IsNull()) return std::nullopt;
    return offset.Shape();
}

/// A shell of the body's own faces, which already share their edges.
TopoDS_Shell ShellOf(const TopTools_IndexedMapOfShape& faces) {
    TopoDS_Shell shell;
    BRep_Builder builder;
    builder.MakeShell(shell);
    for (int i = 1; i <= faces.Extent(); ++i) builder.Add(shell, faces(i));
    return shell;
}

/// Inward shell made of booleans, for bodies the OCCT offset cannot take:
/// the offset walls of two contours run into each other (a slot 2 mm from
/// the outer wall with a 2 mm shell — the tube and the wall touch). The
/// walls are the body in common with the union of slabs `thickness` deep
/// under every face that stays; where two faces meet at a concave edge or
/// vertex, the slab of the faces together fills the sharp corner. Colliding
/// walls simply overlap in the union.
Expected<TopoDS_Shape> ShellBySlabs(const TopoDS_Shape& body, const std::vector<TopoDS_Face>& open, double thickness) {
    TopTools_IndexedMapOfShape removed;
    for (const TopoDS_Face& f : open) removed.Add(f);
    std::vector<TopoDS_Shape> slabs;
    const auto add = [&](const TopoDS_Shape& faces) {
        const std::optional<TopoDS_Shape> slab = Thicken(faces, -thickness);
        if (slab) slabs.push_back(*slab);
        return slab.has_value();
    };
    for (TopExp_Explorer f(body, TopAbs_FACE); f.More(); f.Next())
        if (!removed.Contains(f.Current()) && !add(f.Current())) return Error{kFailed};

    const BRepOffset_Analyse analyse(body, 1.0 * 3.14159265358979 / 180);
    const auto concave = [&](const TopoDS_Shape& edge) {
        const auto& types = analyse.Type(TopoDS::Edge(edge));   // NCollection_List<BRepOffset_Interval> (OCCT 8)
        return !types.IsEmpty() && types.First().Type() == ChFiDS_Concave;
    };
    // The faces around a corner, or nothing if one of them is removed: the
    // corner is open there.
    const auto corner = [&](const TopTools_ListOfShape& around) {
        TopTools_IndexedMapOfShape faces;
        for (const TopoDS_Shape& f : around) {
            if (removed.Contains(f)) return TopTools_IndexedMapOfShape{};
            faces.Add(f);
        }
        return faces;
    };
    TopTools_IndexedDataMapOfShapeListOfShape edgeFaces, vertexEdges, vertexFaces;
    TopExp::MapShapesAndAncestors(body, TopAbs_EDGE, TopAbs_FACE, edgeFaces);
    TopExp::MapShapesAndAncestors(body, TopAbs_VERTEX, TopAbs_EDGE, vertexEdges);
    TopExp::MapShapesAndAncestors(body, TopAbs_VERTEX, TopAbs_FACE, vertexFaces);
    for (int i = 1; i <= edgeFaces.Extent(); ++i) {
        if (!concave(edgeFaces.FindKey(i))) continue;
        const TopTools_IndexedMapOfShape faces = corner(edgeFaces(i));
        if (faces.Extent() == 2 && !add(ShellOf(faces))) return Error{kFailed};
    }
    for (int i = 1; i <= vertexEdges.Extent(); ++i) {
        TopTools_IndexedMapOfShape edges;
        for (const TopoDS_Shape& e : vertexEdges(i))
            if (concave(e)) edges.Add(e);
        if (edges.Extent() < 2) continue;
        const TopTools_IndexedMapOfShape faces = corner(vertexFaces.FindFromKey(vertexEdges.FindKey(i)));
        if (faces.Extent() > 2 && !add(ShellOf(faces))) return Error{kFailed};
    }
    if (slabs.empty()) return Error{kFailed};
    const TopoDS_Shape first = slabs.front();
    slabs.erase(slabs.begin());
    const Expected<TopoDS_Shape> walls = ApplyBoolean(first, slabs, doc::BooleanOp::Join);
    if (!walls) return Error{kFailed};
    return ApplyBoolean(body, *walls, doc::BooleanOp::Intersect);
}

/// Whether the shell made by OCCT can be taken as it is.
bool Accepted(const TopoDS_Shape& body, const TopoDS_Shape& result, bool outward) {
    return !result.IsNull() && BRepCheck_Analyzer(result).IsValid() && IsHollowed(body, result, outward);
}

} // namespace

Expected<TopoDS_Shape> MakeShell(const TopoDS_Shape& body, const std::vector<TopoDS_Face>& open, double thickness, bool outward) {
    if (thickness <= 1e-6) return Error{"Толщина должна быть больше нуля"};
    // Thinner than OCCT's tolerances can follow it fails like a too thick
    // wall did, and the message said "too thick" (0.0001 mm in the tests).
    if (thickness < 1e-3) return Error{"Толщина слишком маленькая (меньше 0,001 мм)"};
    // The OCCT offset cannot shift an edge whose faces go from a corner to
    // tangent along it (a square-to-circle loft). Its simple per-face offset
    // "succeeds" there but gives faces turned the wrong way, so it is not
    // used: the message says why and how to get the part another way.
    const auto failed = [&] { return Error{HasMixedEdges(body) ? kMixedEdges : kFailed}; };
    TopoDS_Shape result;
    try {
        if (open.empty()) {
            // Closed hollow: cut the inner offset from the body (or the body
            // from the outer offset).
            const Expected<TopoDS_Shape> other = Offset(body, outward ? thickness : -thickness);
            if (other) {
                const Expected<TopoDS_Shape> cut =
                    outward ? ApplyBoolean(*other, body, doc::BooleanOp::Cut) : ApplyBoolean(body, *other, doc::BooleanOp::Cut);
                if (cut) result = *cut;
            }
        } else {
            TopTools_ListOfShape faces;
            for (const TopoDS_Face& f : open) faces.Append(f);
            BRepOffsetAPI_MakeThickSolid solid;
            Handle(Deadline) deadline = new Deadline(ops::kBooleanSeconds);
            solid.MakeThickSolidByJoin(body, faces, outward ? thickness : -thickness, kTolerance, BRepOffset_Skin, false,
                                       false, GeomAbs_Intersection, false, Message_ProgressRange(deadline->Start()));
            if (deadline->Passed()) return Error{kTooLong};
            if (solid.IsDone()) result = solid.Shape();
        }
    } catch (const Standard_Failure&) {
        result.Nullify();
    }
    if (Accepted(body, result, outward)) return result;
    // The OCCT offset fails, or "succeeds" with an invalid body, where the
    // inner walls of two contours meet (a slot 2 mm from the outer wall). An
    // inward shell is then made of booleans. Not for the mixed edges of a
    // loft: there the message explains the way around.
    if (outward || HasMixedEdges(body)) return failed();
    try {
        const Expected<TopoDS_Shape> slabs = ShellBySlabs(body, open, thickness);
        if (slabs && Accepted(body, *slabs, outward)) return *slabs;
    } catch (const Standard_Failure&) {
    }
    return Error{kFailed};
}

} // namespace bz::kernel::occt
