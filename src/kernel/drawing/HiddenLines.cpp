#include "kernel/drawing/HiddenLines.h"

#include "kernel/Model.h"
#include "kernel/occt/EdgeMatch.h"
#include "kernel/occt/ModelImpl.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepPrimAPI_MakeHalfSpace.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepLib.hxx>
#include <BRepTools.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <GCPnts_TangentialDeflection.hxx>
#include <HLRAlgo_Projector.hxx>
#include <HLRBRep_Algo.hxx>
#include <HLRBRep_HLRToShape.hxx>
#include <Standard_Failure.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS_Compound.hxx>
#include <gp_Ax2.hxx>
#include <gp_Pln.hxx>
#include <gp_Circ.hxx>
#include <gp_Cylinder.hxx>

#include <algorithm>
#include <cmath>

namespace bz::kernel {

struct ViewShapes::Data {
    TopoDS_Shape shape;
};

namespace {

/// An edge of the HLR result as points in the view's plane.
std::vector<Vec2> Points(const TopoDS_Edge& edge) {
    std::vector<Vec2> out;
    try {
        BRepAdaptor_Curve c(edge);
        if (c.GetType() == GeomAbs_Line) {
            const gp_Pnt a = c.Value(c.FirstParameter()), b = c.Value(c.LastParameter());
            return {{a.X(), a.Y()}, {b.X(), b.Y()}};
        }
        // A curve: pieces turning by under 4.5° and off it by under 5 µm.
        GCPnts_TangentialDeflection points(c, 0.08, 0.005);
        for (int i = 1; i <= points.NbPoints(); ++i) {
            const gp_Pnt p = points.Value(i);
            out.push_back({p.X(), p.Y()});
        }
    } catch (const Standard_Failure&) {
        out.clear();
    }
    return out;
}

void AddEdges(const TopoDS_Shape& shape, std::vector<std::vector<Vec2>>& out) {
    if (shape.IsNull()) return;
    BRepLib::BuildCurves3d(shape);   // the result has its edges as 2D curves on the plane only
    for (TopExp_Explorer e(shape, TopAbs_EDGE); e.More(); e.Next()) {
        std::vector<Vec2> points = Points(TopoDS::Edge(e.Current()));
        if (points.size() < 2) continue;
        const Vec2 d = points.back() - points.front();
        if (points.size() == 2 && std::hypot(d.x, d.y) < 1e-6) continue;   // a point, not a line
        out.push_back(std::move(points));
    }
}

/// Centre lines: of every cylindrical face going at least halfway round
/// (holes, shafts; not fillets), merged by axis.
void AddAxes(const TopoDS_Shape& shape, const gp_Dir& eye, const gp_Dir& right, const gp_Dir& up, ProjectedLines& out) {
    const auto to2d = [&](const gp_Pnt& p) { return Vec2{gp_Vec(p.XYZ()).Dot(gp_Vec(right)), gp_Vec(p.XYZ()).Dot(gp_Vec(up))}; };
    struct Line {
        Vec2   origin, dir;   ///< dir: unit
        double from, to;
    };
    std::vector<Line> lines;
    for (TopExp_Explorer f(shape, TopAbs_FACE); f.More(); f.Next()) {
        const TopoDS_Face& face = TopoDS::Face(f.Current());
        BRepAdaptor_Surface s(face, false);
        if (s.GetType() != GeomAbs_Cylinder) continue;
        double u0, u1, v0, v1;
        BRepTools::UVBounds(face, u0, u1, v0, v1);
        if (u1 - u0 < kPi - 1e-3) continue;   // a fillet or a part of a round
        const gp_Cylinder cyl = s.Cylinder();
        const gp_Ax1 axis = cyl.Axis();
        const double along = std::abs(axis.Direction().Dot(eye));
        if (along > 1 - 1e-6) {   // seen end on: a cross
            const Vec2 c = to2d(axis.Location());
            const auto same = std::find_if(out.marks.begin(), out.marks.end(), [&](const ProjectedLines::Mark& m) {
                return std::hypot(m.center.x - c.x, m.center.y - c.y) < 1e-4;
            });
            if (same == out.marks.end()) out.marks.push_back({c, cyl.Radius()});
            else same->radius = std::max(same->radius, cyl.Radius());
        } else if (along < 1e-6) {   // from the side: its axis from end to end
            const gp_Pnt p0 = axis.Location().Translated(gp_Vec(axis.Direction()) * v0);
            const gp_Pnt p1 = axis.Location().Translated(gp_Vec(axis.Direction()) * v1);
            const Vec2 a = to2d(p0), b = to2d(p1), d = b - a;
            const double len = std::hypot(d.x, d.y);
            if (len < 1e-9) continue;
            const Vec2 dir = d / len;
            bool merged = false;
            for (Line& l : lines) {
                const double cross = l.dir.x * dir.y - l.dir.y * dir.x;
                const Vec2 off = a - l.origin;
                const double distance = std::abs(l.dir.x * off.y - l.dir.y * off.x);
                if (std::abs(cross) > 1e-6 || distance > 1e-4) continue;
                const auto t = [&](Vec2 p) { return (p.x - l.origin.x) * l.dir.x + (p.y - l.origin.y) * l.dir.y; };
                l.from = std::min({l.from, t(a), t(b)});
                l.to = std::max({l.to, t(a), t(b)});
                merged = true;
                break;
            }
            if (!merged) lines.push_back({a, dir, 0, len});
        }
        // Slanted to the view (an isometry): no centre line.
    }
    for (const Line& l : lines) out.axes.push_back({l.origin + l.dir * l.from, l.origin + l.dir * l.to});
}

/// Distance from `p` to the segment a–b.
double Distance(Vec2 p, Vec2 a, Vec2 b) {
    const Vec2 d = b - a;
    const double len2 = d.x * d.x + d.y * d.y;
    double t = len2 > 0 ? ((p.x - a.x) * d.x + (p.y - a.y) * d.y) / len2 : 0;
    t = std::clamp(t, 0.0, 1.0);
    const Vec2 q = a + d * t;
    return std::hypot(p.x - q.x, p.y - q.y);
}

/// ГОСТ 2.305: a hidden line under a visible one is not drawn. HLR gives
/// both (the back face's edges behind the front's): a hidden line all of
/// whose points, ends and middles, lie on visible lines goes.
void DropCovered(std::vector<std::vector<Vec2>>& hidden, const std::vector<std::vector<Vec2>>& visible) {
    Box2 all;
    for (const auto& l : visible)
        for (Vec2 p : l) all.Include(p);
    if (all.IsEmpty()) return;
    const double tolerance = 1e-6 * std::max(1.0, std::max(all.max.x - all.min.x, all.max.y - all.min.y));
    struct Segment {
        Vec2 a, b;
        Box2 box;
    };
    std::vector<Segment> segments;
    for (const auto& l : visible)
        for (std::size_t i = 1; i < l.size(); ++i) {
            Segment s{l[i - 1], l[i], {}};
            s.box.Include(s.a);
            s.box.Include(s.b);
            segments.push_back(s);
        }
    const auto onVisible = [&](Vec2 p) {
        for (const Segment& s : segments) {
            if (p.x < s.box.min.x - tolerance || p.x > s.box.max.x + tolerance || p.y < s.box.min.y - tolerance ||
                p.y > s.box.max.y + tolerance)
                continue;
            if (Distance(p, s.a, s.b) <= tolerance) return true;
        }
        return false;
    };
    std::erase_if(hidden, [&](const std::vector<Vec2>& line) {
        for (std::size_t i = 0; i < line.size(); ++i) {
            if (!onVisible(line[i])) return false;
            if (i > 0 && !onVisible((line[i - 1] + line[i]) * 0.5)) return false;
        }
        return true;
    });
}

} // namespace

ViewShapes ViewShapes::Of(const Model& model) {
    const auto& bodies = model.GetImpl().bodies;
    if (bodies.empty()) return {};
    TopoDS_Compound all;
    BRep_Builder builder;
    builder.MakeCompound(all);
    for (const Body& b : bodies)
        if (!b.shape.IsNull()) builder.Add(all, b.shape);
    auto data = std::make_shared<Data>();
    try {
        data->shape = BRepBuilderAPI_Copy(all, true).Shape();   // its own geometry: another thread may read it
    } catch (const Standard_Failure&) {
        return {};
    }
    ViewShapes out;
    out.myData = std::move(data);
    return out;
}

namespace {

/// The model cut by the plane: what is on the normal's side goes.
Expected<TopoDS_Shape> CutBy(const TopoDS_Shape& shape, const CutPlane& cut) {
    const Vec3 n = cut.normal.Normalized();
    const gp_Pnt p(cut.point.x, cut.point.y, cut.point.z);
    const TopoDS_Face plane = BRepBuilderAPI_MakeFace(gp_Pln(p, gp_Dir(n.x, n.y, n.z))).Face();
    const gp_Pnt away(p.X() + n.x, p.Y() + n.y, p.Z() + n.z);
    BRepPrimAPI_MakeHalfSpace half(plane, away);
    BRepAlgoAPI_Cut op(shape, half.Solid());
    if (!op.IsDone() || op.Shape().IsNull()) return Error{"Разрез не построился: ядро не смогло рассечь модель"};
    return op.Shape();
}

/// The faces of `shape` on the cutting plane, as loops in the view's plane.
std::vector<std::vector<std::vector<Vec2>>> SectionFaces(const TopoDS_Shape& shape, const CutPlane& cut, const Vec3& right, const Vec3& up) {
    const Vec3 n = cut.normal.Normalized();
    const auto to2d = [&](const gp_Pnt& q) { return Vec2{q.X() * right.x + q.Y() * right.y + q.Z() * right.z, q.X() * up.x + q.Y() * up.y + q.Z() * up.z}; };
    std::vector<std::vector<std::vector<Vec2>>> out;
    for (TopExp_Explorer f(shape, TopAbs_FACE); f.More(); f.Next()) {
        const TopoDS_Face& face = TopoDS::Face(f.Current());
        BRepAdaptor_Surface s(face, false);
        if (s.GetType() != GeomAbs_Plane) continue;
        const gp_Pln pl = s.Plane();
        const gp_Dir d = pl.Axis().Direction();
        if (std::abs(d.X() * n.x + d.Y() * n.y + d.Z() * n.z) < 1 - 1e-6) continue;
        const gp_Pnt o = pl.Location();
        const double off = (o.X() - cut.point.x) * n.x + (o.Y() - cut.point.y) * n.y + (o.Z() - cut.point.z) * n.z;
        if (std::abs(off) > 1e-5) continue;
        std::vector<std::vector<Vec2>> loops;
        for (TopExp_Explorer w(face, TopAbs_WIRE); w.More(); w.Next()) {
            std::vector<Vec2> loop;
            for (BRepTools_WireExplorer e(TopoDS::Wire(w.Current()), face); e.More(); e.Next()) {
                BRepAdaptor_Curve c(e.Current());
                std::vector<Vec2> pts;
                if (c.GetType() == GeomAbs_Line) {
                    pts = {to2d(c.Value(c.FirstParameter())), to2d(c.Value(c.LastParameter()))};
                } else {
                    GCPnts_TangentialDeflection points(c, 0.08, 0.005);
                    for (int i = 1; i <= points.NbPoints(); ++i) pts.push_back(to2d(points.Value(i)));
                }
                if (e.Current().Orientation() == TopAbs_REVERSED) std::reverse(pts.begin(), pts.end());
                for (std::size_t i = loop.empty() ? 0 : 1; i < pts.size(); ++i) loop.push_back(pts[i]);
            }
            if (loop.size() >= 3) loops.push_back(std::move(loop));
        }
        if (!loops.empty()) out.push_back(std::move(loops));
    }
    return out;
}

} // namespace

Expected<ProjectedLines> ViewShapes::Project(Vec3 eye, Vec3 up, const std::optional<CutPlane>& cut) const {
    if (!myData) return Error{"В модели нет тел"};
    const Vec3 e = eye.Normalized(), u = up.Normalized(), r = u.Cross(e).Normalized();
    if (r.Length() < 0.5) return Error{"Неверное направление вида"};
    ProjectedLines out;
    try {
        TopoDS_Shape shape = myData->shape;
        if (cut) {
            Expected<TopoDS_Shape> cutShape = CutBy(shape, *cut);
            if (!cutShape) return cutShape.GetError();
            shape = *cutShape;
            out.sections = SectionFaces(shape, *cut, r, u);
        }
        const gp_Dir eyeDir(e.x, e.y, e.z), rightDir(r.x, r.y, r.z), upDir(u.x, u.y, u.z);
        // The view's plane through the origin: its normal toward the eye, x to the right.
        const gp_Ax2 plane(gp::Origin(), eyeDir, rightDir);
        Handle(HLRBRep_Algo) algo = new HLRBRep_Algo();
        algo->Add(shape);
        algo->Projector(HLRAlgo_Projector(plane));
        algo->Update();
        algo->Hide();
        HLRBRep_HLRToShape result(algo);
        AddEdges(result.VCompound(), out.visible);
        AddEdges(result.OutLineVCompound(), out.visible);
        AddEdges(result.HCompound(), out.hidden);
        AddEdges(result.OutLineHCompound(), out.hidden);
        DropCovered(out.hidden, out.visible);
        AddAxes(shape, eyeDir, rightDir, upDir, out);
    } catch (const Standard_Failure&) {
        return Error{"Вид не построился: ядро не смогло убрать невидимые линии"};
    }
    return out;
}

namespace {

ModelEdge EdgeOf(const TopoDS_Edge& edge) {
    ModelEdge out;
    BRepAdaptor_Curve c(edge);
    const gp_Pnt a = c.Value(c.FirstParameter()), b = c.Value(c.LastParameter());
    out.a = {a.X(), a.Y(), a.Z()};
    out.b = {b.X(), b.Y(), b.Z()};
    if (c.GetType() == GeomAbs_Line) {
        out.kind = ModelEdge::Kind::Line;
    } else if (c.GetType() == GeomAbs_Circle) {
        const gp_Circ circle = c.Circle();
        out.kind   = ModelEdge::Kind::Circle;
        out.center = {circle.Location().X(), circle.Location().Y(), circle.Location().Z()};
        out.axis   = {circle.Axis().Direction().X(), circle.Axis().Direction().Y(), circle.Axis().Direction().Z()};
        out.radius = circle.Radius();
    }
    return out;
}

} // namespace

std::optional<PickedEdge> ViewShapes::EdgeAt(Vec3 eye, Vec3 up, Vec2 p, double tolerance) const {
    if (!myData) return std::nullopt;
    const Vec3 e = eye.Normalized(), u = up.Normalized(), r = u.Cross(e).Normalized();
    const auto to2d = [&](const gp_Pnt& q) { return Vec2{q.X() * r.x + q.Y() * r.y + q.Z() * r.z, q.X() * u.x + q.Y() * u.y + q.Z() * u.z}; };
    TopTools_IndexedMapOfShape edges;
    TopExp::MapShapes(myData->shape, TopAbs_EDGE, edges);
    std::optional<TopoDS_Edge> best;
    double bestDistance = tolerance, bestDepth = -1e300;
    try {
        for (int i = 1; i <= edges.Extent(); ++i) {
            const TopoDS_Edge& edge = TopoDS::Edge(edges(i));
            if (BRep_Tool::Degenerated(edge)) continue;
            BRepAdaptor_Curve c(edge);
            std::vector<gp_Pnt> points;
            if (c.GetType() == GeomAbs_Line) {
                points = {c.Value(c.FirstParameter()), c.Value(c.LastParameter())};
            } else {
                GCPnts_TangentialDeflection d(c, 0.08, 0.01);
                for (int k = 1; k <= d.NbPoints(); ++k) points.push_back(d.Value(k));
            }
            double distance = 1e300, depth = -1e300;
            for (std::size_t k = 0; k + 1 < points.size(); ++k) {
                const double l = Distance(p, to2d(points[k]), to2d(points[k + 1]));
                if (l < distance) {
                    distance = l;
                    const gp_Pnt m((points[k].XYZ() + points[k + 1].XYZ()) / 2);
                    depth = m.X() * e.x + m.Y() * e.y + m.Z() * e.z;   // toward the eye
                }
            }
            // Edges seen on top of each other (a box's front and back): the nearer one.
            const double same = 1e-3 * std::max(1.0, tolerance);
            if (distance < bestDistance - same || (distance <= bestDistance + same && depth > bestDepth && distance <= tolerance)) {
                bestDistance = std::min(bestDistance, distance);
                bestDepth = depth;
                best = edge;
            }
        }
        if (!best) return std::nullopt;
        return PickedEdge{occt::DescribeEdge(*best, myData->shape), EdgeOf(*best)};
    } catch (const Standard_Failure&) {
        return std::nullopt;
    }
}

std::vector<std::optional<ModelEdge>> ViewShapes::Resolve(const std::vector<doc::EdgeReference>& refs) const {
    std::vector<std::optional<ModelEdge>> out(refs.size());
    if (!myData || refs.empty()) return out;
    try {
        const std::vector<std::optional<TopoDS_Edge>> found = occt::ResolveEdges(refs, myData->shape);
        for (std::size_t i = 0; i < found.size() && i < out.size(); ++i)
            if (found[i]) out[i] = EdgeOf(*found[i]);
    } catch (const Standard_Failure&) {
        return std::vector<std::optional<ModelEdge>>(refs.size());
    }
    return out;
}

} // namespace bz::kernel
