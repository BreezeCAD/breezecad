#include "kernel/occt/Fillet.h"
#include "kernel/occt/Ops.h"

#include <BRepCheck_Analyzer.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <BRepFilletAPI_MakeChamfer.hxx>
#include <BRepLProp_SLProps.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRep_Tool.hxx>
#include <Geom2d_Curve.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopoDS.hxx>
#include <BRepFilletAPI_MakeFillet.hxx>
#include <Standard_Failure.hxx>
#include <format>

#include "core/Log.h"
#include "kernel/occt/EdgeMatch.h"
#include "core/Math.h"
#include <BRepGProp.hxx>
#include <ChFi3d.hxx>
#include <GCPnts_AbscissaPoint.hxx>
#include <GProp_GProps.hxx>
#include <ShapeFix_Shape.hxx>

#include <algorithm>
#include <chrono>
#include <optional>
#include <array>
#include <cmath>

namespace bz::kernel::occt {
namespace {

/// Outward normal of `face` where `edge` runs along it, at the edge's middle.
gp_Dir NormalAlong(const TopoDS_Edge& edge, const TopoDS_Face& face);

double VolumeOf(const TopoDS_Shape& shape) {
    GProp_GProps props;
    BRepGProp::VolumeProperties(shape, props);
    return props.Mass();
}

/// The two faces on either side of `edge` (one, for a seam).
std::vector<TopoDS_Face> FacesOf(const TopoDS_Edge& edge, const TopTools_IndexedDataMapOfShapeListOfShape& faces) {
    std::vector<TopoDS_Face> two;
    if (!faces.Contains(edge)) return two;
    for (const TopoDS_Shape& f : faces.FindFromKey(edge))
        if (std::ranges::none_of(two, [&](const TopoDS_Face& t) { return t.IsSame(f); })) two.push_back(TopoDS::Face(f));
    return two;
}

/// What an edge between two faces is like where a fillet or chamfer goes.
struct EdgeShape {
    bool   smooth = false;   ///< the faces meet without an angle: nothing to round or bevel
    bool   convex = true;    ///< an outer edge (material is taken away) or an inner one (added)
    double opening = kPi / 2;   ///< the angle of the empty wedge the operation fills or cuts, rad
    double length = 0;
};

EdgeShape ShapeOf(const TopoDS_Edge& edge, const TopTools_IndexedDataMapOfShapeListOfShape& faces) {
    EdgeShape out;
    BRepAdaptor_Curve curve(edge);
    out.length = GCPnts_AbscissaPoint::Length(curve);
    const std::vector<TopoDS_Face> two = FacesOf(edge, faces);
    if (two.size() != 2) return out;
    try {
        if (ChFi3d::IsTangentFaces(edge, two[0], two[1])) {
            out.smooth = true;
            return out;
        }
        out.convex = ChFi3d::DefineConnectType(edge, two[0], two[1], 1e-6, false) != ChFiDS_Concave;
    } catch (const Standard_Failure&) {
    }
    const double between = NormalAlong(edge, two[0]).Angle(NormalAlong(edge, two[1]));   // 0 — the faces go on flat
    out.opening = std::clamp(kPi - between, 0.05, kPi - 0.05);
    return out;
}

/// Runs a fillet-like builder and checks the result.
///
/// OCCT builds a fillet or chamfer that stops where its edge stops (at a
/// corner of three faces, the edge of a seat for a round glass running out
/// into the slot's wall) but often leaves the end of it not quite closed:
/// one wire of a face open by a hair. The shape is then not valid and was
/// thrown away with "the radius is too big" — whatever the radius. Such a
/// shape is healed (ShapeFix) and kept if the material it gained or lost is
/// what rounding those edges takes: `section(edge)` is that, mm², per
/// millimetre of each edge (concave edges gain it, convex lose).
template <class Builder, class AddEdge, class Section>
Expected<TopoDS_Shape> Build(const TopoDS_Shape& body, const std::vector<TopoDS_Edge>& edges, double size, AddEdge add, Section section,
                             const char* what, const char* tooBig) {
    if (edges.empty()) return Error{std::format("Выберите рёбра для операции «{}»", what)};
    if (size <= 1e-6) return Error{"Размер должен быть больше нуля"};
    TopTools_IndexedDataMapOfShapeListOfShape faces;
    TopExp::MapShapesAndAncestors(body, TopAbs_EDGE, TopAbs_FACE, faces);
    // An edge between faces that meet without an angle (a cylinder going
    // on into a plane along its side) has nothing to round: say which it is
    // instead of "too big".
    for (const TopoDS_Edge& e : edges)
        if (ShapeOf(e, faces).smooth) {
            BRepAdaptor_Curve c(e);
            const gp_Pnt m = c.Value((c.FirstParameter() + c.LastParameter()) / 2);
            return Error{std::format("Ребро у точки ({:.2f}; {:.2f}; {:.2f}) гладкое: грани переходят одна в другую без угла — «{}» там не нужно, "
                                     "уберите его из выбора",
                                     m.X(), m.Y(), m.Z(), what)};
        }
    try {
        Builder builder(body);
        for (const TopoDS_Edge& e : edges) add(builder, e);
        Handle(Deadline) deadline = new Deadline(ops::kBooleanSeconds);
        builder.Build(Message_ProgressRange(deadline->Start()));
        if (deadline->Passed())
            return Error{std::format("«{}» не уложилось в {:.0f} с — выберите меньше рёбер за раз или уменьшите размер", what,
                                     ops::kBooleanSeconds)};
        if (!builder.IsDone() || builder.Shape().IsNull()) {
            // Where it gave up, when OCCT says: the user sees which corner.
            if constexpr (requires { builder.NbFaultyVertices(); }) {
                if (builder.NbFaultyVertices() > 0) {
                    const gp_Pnt p = BRep_Tool::Pnt(builder.FaultyVertex(1));
                    return Error{std::format("{} — не сходится у вершины ({:.2f}; {:.2f}; {:.2f}): там ребро обрывается о другую грань. "
                                             "Попробуйте добавить в выбор соседние рёбра у этой вершины или уменьшить размер",
                                             tooBig, p.X(), p.Y(), p.Z())};
                }
            }
            return Error{tooBig};
        }
        TopoDS_Shape result = builder.Shape();
        if (BRepCheck_Analyzer(result).IsValid()) return result;

        // The edges the operation went along: those chosen and those it
        // carried on to over smooth joints.
        double expected = 0, scale = 0;
        for (int c = 1; c <= builder.NbContours(); ++c)
            for (int i = 1; i <= builder.NbEdges(c); ++i) {
                const TopoDS_Edge& e = builder.Edge(c, i);
                const EdgeShape shape = ShapeOf(e, faces);
                const double v = section(e, shape) * shape.length;
                expected += shape.convex ? -v : v;
                scale += std::abs(v);
            }
        Handle(ShapeFix_Shape) fix = new ShapeFix_Shape(result);
        fix->Perform(Message_ProgressRange(deadline->Start()));
        const TopoDS_Shape healed = fix->Shape();
        if (healed.IsNull() || !BRepCheck_Analyzer(healed).IsValid()) return Error{tooBig};
        const double gained = VolumeOf(healed) - VolumeOf(body);
        if (std::abs(gained - expected) > 0.35 * scale + 1e-4) {
            log::Warn("{}: healed result rejected, volume change {:.4f} mm3 where {:.4f} was expected", what, gained, expected);
            return Error{tooBig};
        }
        log::Info("{}: OCCT left the result open at an edge's end; healed (volume change {:.4f} mm3, expected {:.4f})", what, gained,
                  expected);
        return healed;
    } catch (const Standard_Failure& failure) {
        // OCCT gave up inside, typically where the chosen edge stops against
        // another face: the whole outline of the pocket often builds when
        // one edge of it does not.
        log::Warn("{}: OCCT failed inside ({})", what, ops::TextOf(failure));
        return Error{std::format("{} Похоже, ребро обрывается у вершины, где OCCT не может закрыть конец: выберите вместе с ним соседние "
                                 "рёбра того же контура (например, весь край паза) или уменьшите размер",
                                 tooBig)};
    }
}

gp_Dir NormalAlong(const TopoDS_Edge& edge, const TopoDS_Face& face) {
    double first = 0, last = 0;
    const Handle(Geom2d_Curve) pcurve = BRep_Tool::CurveOnSurface(edge, face, first, last);
    if (pcurve.IsNull()) return gp_Dir(0, 0, 1);
    const gp_Pnt2d uv = pcurve->Value((first + last) / 2);
    const BRepAdaptor_Surface surface(face);
    BRepLProp_SLProps props(surface, uv.X(), uv.Y(), 1, 1e-7);
    if (!props.IsNormalDefined()) return gp_Dir(0, 0, 1);
    gp_Dir n = props.Normal();
    if (face.Orientation() == TopAbs_REVERSED) n.Reverse();
    return n;
}

/// The reference face of `edge` for unequal chamfer sizes (see MakeChamfer).
TopoDS_Face ReferenceFace(const TopoDS_Edge& edge, const TopTools_IndexedDataMapOfShapeListOfShape& faces, bool flipped) {
    const TopTools_ListOfShape& around = faces.FindFromKey(edge);
    std::vector<TopoDS_Face> two;
    for (const TopoDS_Shape& f : around)
        if (std::ranges::none_of(two, [&](const TopoDS_Face& t) { return t.IsSame(f); })) two.push_back(TopoDS::Face(f));
    if (two.size() < 2) return two.empty() ? TopoDS_Face() : two.front();
    const auto key = [&](const TopoDS_Face& f) {
        const gp_Dir n = NormalAlong(edge, f);
        return std::array<double, 3>{n.Z(), n.Y(), n.X()};
    };
    const auto a = key(two[0]), b = key(two[1]);
    bool firstIsUpper = true;
    for (std::size_t i = 0; i < 3; ++i)
        if (std::abs(a[i] - b[i]) > 1e-6) {
            firstIsUpper = a[i] > b[i];
            break;
        }
    return firstIsUpper != flipped ? two[0] : two[1];
}

} // namespace

namespace {

Expected<TopoDS_Shape> FilletOnce(const TopoDS_Shape& body, const std::vector<TopoDS_Edge>& edges, double radius) {
    return Build<BRepFilletAPI_MakeFillet>(
        body, edges, radius, [radius](BRepFilletAPI_MakeFillet& b, const TopoDS_Edge& e) { b.Add(radius, e); },
        [radius](const TopoDS_Edge&, const EdgeShape& s) {
            return radius * radius * (1.0 / std::tan(s.opening / 2) - (kPi - s.opening) / 2);
        },
        "Сопряжение", "Не удалось построить сопряжение: радиус слишком большой для этих рёбер?");
}

std::string Where(const doc::EdgeReference& e) { return std::format("({:.2f}; {:.2f}; {:.2f})", e.middle.x, e.middle.y, e.middle.z); }

} // namespace

Expected<TopoDS_Shape> MakeFillet(const TopoDS_Shape& body, const std::vector<TopoDS_Edge>& edges, double radius, std::string* warning) {
    // The fallbacks below rebuild the fillet several times: only while that
    // stays cheap (a rebuild runs on every change further up the tree).
    const auto started = std::chrono::steady_clock::now();
    Expected<TopoDS_Shape> all = FilletOnce(body, edges, radius);
    const double once = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    if (all || edges.size() < 2) {
        if (!all && edges.size() == 1 && once * 7 < 1.0) {
            // One edge: how large a radius it takes (a few tries, only on failure).
            double lo = 0, hi = radius;
            for (int i = 0; i < 7; ++i) {
                const double mid = (lo + hi) / 2;
                if (FilletOnce(body, edges, mid)) lo = mid;
                else hi = mid;
            }
            if (lo > 1e-3 && lo < radius * 0.98)
                return Error{std::format("{} Для этого ребра строится радиус до ≈{:.2f} мм.", all.GetError().message, lo)};
        }
        return all;
    }
    if (once * static_cast<double>(edges.size()) * 2 > 4.0) return all;
    // The edges one at a time: each is found again on the body rounded so far.
    std::vector<doc::EdgeReference> refs;
    for (const TopoDS_Edge& e : edges) refs.push_back(DescribeEdge(e, body));
    TopoDS_Shape shape = body;
    std::vector<std::size_t> pending(edges.size());
    for (std::size_t i = 0; i < pending.size(); ++i) pending[i] = i;
    for (int pass = 0; pass < 2 && !pending.empty(); ++pass) {   // a second pass: some build once their neighbours are round
        std::vector<std::size_t> left;
        for (const std::size_t i : pending) {
            const std::optional<TopoDS_Edge> e = ResolveEdges({refs[i]}, shape).front();
            if (!e) {
                left.push_back(i);
                continue;
            }
            // The edge found again must still be where it was (not a look-alike elsewhere).
            const doc::EdgeReference now = DescribeEdge(*e, shape);
            if (now.curve != refs[i].curve || (now.middle - refs[i].middle).Length() > std::max(radius * 2, 0.5)) {
                left.push_back(i);
                continue;
            }
            if (Expected<TopoDS_Shape> one = FilletOnce(shape, {*e}, radius)) shape = *one;
            else left.push_back(i);
        }
        if (left.size() == pending.size()) break;
        pending = std::move(left);
    }
    if (pending.size() == edges.size()) return all;   // none on its own either: the first message says why
    if (!pending.empty()) {
        std::string where;
        for (const std::size_t i : pending) where += (where.empty() ? "" : ", ") + Where(refs[i]);
        log::Warn("Сопряжение: {} of {} edges built one by one, not built at {}", edges.size() - pending.size(), edges.size(), where);
        if (warning)
            *warning = std::format("Сопряжение построено на {} рёбрах из {}: не строятся рёбра у {} — уменьшите радиус или уберите их из выбора",
                                   edges.size() - pending.size(), edges.size(), where);
    } else {
        log::Info("Сопряжение: built edge by edge ({} edges) where all at once failed", edges.size());
    }
    return shape;
}

Expected<TopoDS_Shape> MakeChamfer(const TopoDS_Shape& body, const std::vector<TopoDS_Edge>& edges, double distance) {
    return Build<BRepFilletAPI_MakeChamfer>(
        body, edges, distance, [distance](BRepFilletAPI_MakeChamfer& b, const TopoDS_Edge& e) { b.Add(distance, e); },
        [distance](const TopoDS_Edge&, const EdgeShape& s) { return distance * distance * std::sin(s.opening) / 2; }, "Фаска",
        "Не удалось построить фаску: расстояние слишком большое для этих рёбер?");
}

Expected<TopoDS_Shape> MakeChamfer(const TopoDS_Shape& body, const std::vector<TopoDS_Edge>& edges, const ChamferSize& size) {
    using doc::ChamferMode;
    if (size.mode == ChamferMode::EqualDistance) return MakeChamfer(body, edges, size.distance);
    if (size.mode == ChamferMode::TwoDistances && size.distance2 <= 1e-6) return Error{"Размер должен быть больше нуля"};
    if (size.mode == ChamferMode::DistanceAngle && (size.angle <= 1e-6 || size.angle >= kPi / 2 - 1e-6))
        return Error{"Угол фаски должен быть больше 0° и меньше 90°"};
    TopTools_IndexedDataMapOfShapeListOfShape faces;
    TopExp::MapShapesAndAncestors(body, TopAbs_EDGE, TopAbs_FACE, faces);
    std::size_t next = 0;   // Build adds the edges in order
    return Build<BRepFilletAPI_MakeChamfer>(
        body, edges, size.distance,
        [&](BRepFilletAPI_MakeChamfer& b, const TopoDS_Edge& e) {
            const std::size_t i = next++;
            if (!faces.Contains(e)) return;
            const bool own = i < size.flips.size() && size.flips[i];
            const TopoDS_Face face = ReferenceFace(e, faces, size.flipped != own);
            if (face.IsNull()) return;
            if (size.mode == ChamferMode::TwoDistances) b.Add(size.distance, size.distance2, e, face);
            else b.AddDA(size.distance, size.angle, e, face);
        },
        [&size](const TopoDS_Edge&, const EdgeShape& s) {
            // The bevel's triangle: `distance` on one face, the other side
            // given or found from the angle at the reference face.
            double other = size.distance2;
            if (size.mode == ChamferMode::DistanceAngle)
                other = size.distance * std::sin(size.angle) / std::max(1e-6, std::sin(kPi - s.opening - size.angle));
            return size.distance * other * std::sin(s.opening) / 2;
        },
        "Фаска", "Не удалось построить фаску: размер слишком большой для этих рёбер?");
}

} // namespace bz::kernel::occt
