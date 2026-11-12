#include "kernel/occt/Extrude.h"
#include "kernel/occt/ShareSurfaces.h"

#include "core/FaultGuard.h"
#include "core/Log.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepBndLib.hxx>
#include <Bnd_Box.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <gp_Pnt2d.hxx>
#include <Geom_ConicalSurface.hxx>
#include <Geom_Surface.hxx>
#include <Geom_RectangularTrimmedSurface.hxx>
#include <BRep_Tool.hxx>
#include <BRepTools.hxx>
#include <BRepClass_FaceClassifier.hxx>
#include <BRepClass3d_SolidClassifier.hxx>
#include <BRepAlgoAPI_Splitter.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <gp_Pln.hxx>
#include <TopoDS_Solid.hxx>
#include <BRepPrimAPI_MakeHalfSpace.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <BRepGProp_Face.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <GProp_GProps.hxx>
#include <IntCurvesFace_ShapeIntersector.hxx>
#include <LocOpe_DPrism.hxx>
#include <Precision.hxx>
#include <gp_Cylinder.hxx>
#include <gp_Lin.hxx>
#include <ShapeUpgrade_UnifySameDomain.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS.hxx>
#include <format>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>

#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

namespace bz::kernel::occt {
namespace {

bool IsEmpty(const TopoDS_Shape& shape) {
    if (shape.IsNull()) return true;
    TopExp_Explorer it(shape, TopAbs_SOLID);
    return !it.More();
}

/// One face swept along `dir` by `length`, optionally with a draft angle.
TopoDS_Shape Sweep(const TopoDS_Face& face, const gp_Vec& dir, double length, double taper) {
    if (std::abs(taper) < 1e-9) return BRepPrimAPI_MakePrism(face, dir * length).Shape();

    // LocOpe_DPrism sweeps along the face normal; orient the face so its
    // normal points where we want to go. Positive taper narrows the solid.
    TopoDS_Face oriented = face;
    BRepGProp_Face props(face);
    double u1, u2, v1, v2;
    props.Bounds(u1, u2, v1, v2);
    gp_Pnt p;
    gp_Vec normal;
    props.Normal((u1 + u2) / 2, (v1 + v2) / 2, p, normal);
    if (normal.Dot(dir) < 0) oriented.Reverse();
    // LocOpe_DPrism measures its height along the slanted side, not along
    // the normal: ask for the slant that ends `length` above the face.
    LocOpe_DPrism prism(oriented, length / std::cos(taper), taper);
    TopoDS_Shape solid = prism.Shape();
    if (solid.IsNull()) return solid;
    // LocOpe_DPrism goes where the face's surface looks, whichever way the
    // face itself is turned: a «reversed» extrusion with a taper was built
    // on the wrong side of the sketch — inside the body, so a join added
    // nothing and said "ok" (BETA-BUGS №6). Built there, it is mirrored in
    // the sketch plane: the same solid on the side asked for.
    GProp_GProps volume;
    BRepGProp::VolumeProperties(solid, volume);
    if (gp_Vec(p, volume.CentreOfMass()).Dot(dir) < 0) {
        gp_Trsf mirror;
        mirror.SetMirror(gp_Ax2(p, gp_Dir(dir)));
        solid = BRepBuilderAPI_Transform(solid, mirror, true).Shape();
    }
    return solid;
}

/// A point inside `face`, nudged `offset` along `dir` (into the solid swept from it).
gp_Pnt PointInside(const TopoDS_Face& face, const gp_Vec& dir, double offset) {
    GProp_GProps props;
    BRepGProp::SurfaceProperties(face, props);
    gp_Pnt centre = props.CentreOfMass();
    BRepClass_FaceClassifier inside(face, centre, 1e-7);
    if (inside.State() != TopAbs_IN) {   // a ring, an L: look for a point on a grid
        BRepAdaptor_Surface surface(face);
        double u1, u2, v1, v2;
        BRepTools::UVBounds(face, u1, u2, v1, v2);
        for (int i = 1; i < 16 && inside.State() != TopAbs_IN; ++i)
            for (int j = 1; j < 16; ++j) {
                const gp_Pnt2d uv(u1 + (u2 - u1) * i / 16.0, v1 + (v2 - v1) * j / 16.0);
                BRepClass_FaceClassifier at(face, uv, 1e-7);
                if (at.State() == TopAbs_IN) {
                    centre = surface.Value(uv.X(), uv.Y());
                    inside.Perform(face, centre, 1e-7);
                    break;
                }
            }
    }
    return centre.Translated(dir.Normalized() * offset);
}

/// The prism swept from `profiles` cut off by `face` (a curved one): the
/// part on the sketch's side. The face as it is first; if it does not
/// cover the prism, its surface carried on beyond the face's edges.
Expected<TopoDS_Shape> CutToFace(const TopoDS_Shape& prism, const TopoDS_Face& face, const std::vector<TopoDS_Face>& profiles,
                                 const gp_Vec& dir, double reach) {
    std::vector<gp_Pnt> seeds;
    for (const TopoDS_Face& p : profiles) seeds.push_back(PointInside(p, dir, 1e-3));
    GProp_GProps whole;
    BRepGProp::VolumeProperties(prism, whole);

    std::vector<TopoDS_Shape> cutters{face};
    Handle(Geom_Surface) surface = BRep_Tool::Surface(face);
    if (const auto trimmed = Handle(Geom_RectangularTrimmedSurface)::DownCast(surface)) surface = trimmed->BasisSurface();
    double u1, u2, v1, v2;
    surface->Bounds(u1, u2, v1, v2);
    const auto clamp = [&](double& lo, double& hi) {
        lo = std::max(lo, -reach);
        hi = std::min(hi, reach);
    };
    clamp(u1, u2);
    clamp(v1, v2);
    // A cone carried on goes no further than its apex, as in Inventor: past
    // it the surface is a second cone turned the other way, and the prism
    // cut by both kept the part beyond the apex too — a long inverted cone
    // hanging under the right one, joined to it at the apex (22.10.2026: a
    // funnel filled down to its cone came out as a "hat" 450 mm deep).
    if (const auto cone = Handle(Geom_ConicalSurface)::DownCast(surface)) {
        const double sine = std::sin(cone->SemiAngle());
        if (std::abs(sine) > 1e-12) {
            const double apex = -cone->RefRadius() / sine;   // V where the radius is zero
            double fu1, fu2, fv1, fv2;                         // the face's own side of the apex
            BRepTools::UVBounds(face, fu1, fu2, fv1, fv2);
            if ((fv1 + fv2) / 2 >= apex) v1 = std::max(v1, apex);
            else v2 = std::min(v2, apex);
        }
    }
    BRepBuilderAPI_MakeFace grown(surface, u1, u2, v1, v2, 1e-7);
    if (grown.IsDone()) cutters.push_back(grown.Face());

    for (const TopoDS_Shape& cutter : cutters) {
        BRepAlgoAPI_Splitter splitter;
        TopTools_ListOfShape arguments, tools;
        arguments.Append(prism);
        tools.Append(cutter);
        splitter.SetArguments(arguments);
        splitter.SetTools(tools);
        splitter.Build();
        if (!splitter.IsDone()) continue;
        TopoDS_Shape kept;
        double volume = 0;
        for (TopExp_Explorer e(splitter.Shape(), TopAbs_SOLID); e.More(); e.Next()) {
            BRepClass3d_SolidClassifier classifier(e.Current());
            const bool start = std::any_of(seeds.begin(), seeds.end(), [&](const gp_Pnt& seed) {
                classifier.Perform(seed, 1e-6);
                return classifier.State() == TopAbs_IN;
            });
            if (!start) continue;
            GProp_GProps props;
            BRepGProp::VolumeProperties(e.Current(), props);
            volume += props.Mass();
            kept = kept.IsNull() ? e.Current() : BRepAlgoAPI_Fuse(kept, e.Current()).Shape();
        }
        if (!kept.IsNull() && volume < whole.Mass() * (1 - 1e-6)) return kept;
    }
    return Error{"Грань «до» не перекрывает выдавливание: выберите другую грань или плоскость"};
}

} // namespace

Expected<TopoDS_Shape> MakeExtrusion(const std::vector<TopoDS_Face>& faces, const doc::ExtrudeFeature& f,
                                     const Frame& frame, double throughAllLength, const std::optional<Frame>& target,
                                     const std::optional<TopoDS_Face>& untilFace) {
    if (faces.empty()) return Error{"Нет замкнутого контура для выдавливания"};
    if (f.extent == doc::ExtentType::ToPlane && untilFace) {
        // Up to a curved face: far enough towards it, then cut off by it.
        // Towards the place where the face was picked (as it is now): the
        // centre of mass of a curved face can be anywhere — on a cylinder's
        // axis, in the sketch plane, above a trough's bottom.
        gp_Pnt c;
        bool found = false;
        if (f.toFace.IsSet()) {
            const Vec3 p = f.toFace.point;
            try {
                BRepExtrema_DistShapeShape nearest(BRepBuilderAPI_MakeVertex(gp_Pnt(p.x, p.y, p.z)).Vertex(), *untilFace);
                if (nearest.IsDone() && nearest.NbSolution() > 0) {
                    c = nearest.PointOnShape2(1);
                    found = true;
                }
            } catch (const Standard_Failure&) {
                // the centre of mass below
            }
        }
        if (!found) {
            GProp_GProps props;
            BRepGProp::SurfaceProperties(*untilFace, props);
            c = props.CentreOfMass();
        }
        const double gap = (Vec3{c.X(), c.Y(), c.Z()} - frame.origin).Dot(frame.normal);
        gp_Vec dir(frame.normal.x, frame.normal.y, frame.normal.z);
        if (gap < 0) dir.Reverse();
        const double reach = std::max(throughAllLength * 2, std::abs(gap) * 2 + 1);
        try {
            TopoDS_Shape prism;
            for (const TopoDS_Face& face : faces) {
                const TopoDS_Shape piece = Sweep(face, dir, reach, f.taper.value);
                if (IsEmpty(piece)) return Error{"Не удалось построить выдавливание (уклон слишком большой?)"};
                prism = prism.IsNull() ? piece : BRepAlgoAPI_Fuse(prism, piece).Shape();
            }
            const Expected<TopoDS_Shape> cut = CutToFace(prism, *untilFace, faces, dir, reach * 2);
            if (!cut) return cut;
            return Clean(*cut);
        } catch (const Standard_Failure& failure) {
            return Error{std::format("Ошибка выдавливания: {}", failure.GetMessageString())};
        }
    }

    double length = f.distance.value;
    if (f.extent == doc::ExtentType::ThroughAll) length = throughAllLength;
    gp_Vec dir(frame.normal.x, frame.normal.y, frame.normal.z);
    if (f.reversed) dir.Reverse();

    // Up to a plane: parallel to the sketch, exactly that far (the side it is
    // on decides the direction); at an angle, far enough and then cut by it,
    // so the end takes the plane's slope.
    std::optional<gp_Pln> slanted;
    if (f.extent == doc::ExtentType::ToPlane) {
        if (!target) return Error{"Выберите плоскость или грань, до которой выдавливать"};
        const double across = target->normal.Dot(frame.normal);
        if (std::abs(across) < 1e-6) return Error{"Плоскость «до» параллельна направлению выдавливания — до неё не дойти"};
        if (std::abs(std::abs(across) - 1.0) < 1e-9) {
            const double gap = (target->origin - frame.origin).Dot(frame.normal);
            if (std::abs(gap) < 1e-6) return Error{"Плоскость «до» совпадает с плоскостью эскиза"};
            dir = gp_Vec(frame.normal.x, frame.normal.y, frame.normal.z) * (gap < 0 ? -1.0 : 1.0);
            length = std::abs(gap);
        } else {
            length = throughAllLength * 2;
            slanted = gp_Pln(gp_Pnt(target->origin.x, target->origin.y, target->origin.z),
                             gp_Dir(target->normal.x, target->normal.y, target->normal.z));
        }
    }
    if (length <= 1e-6) return Error{"Расстояние должно быть больше нуля"};

    // Symmetric starts half the length behind the sketch plane; asymmetric
    // starts `distance2` behind it and goes `distance` beyond it.
    double behind = 0.0;
    if (f.extent == doc::ExtentType::Symmetric) behind = length / 2.0;
    if (f.extent == doc::ExtentType::Asymmetric) {
        if (f.distance2.value <= 1e-6) return Error{"Расстояние должно быть больше нуля"};
        behind = f.distance2.value;
        length += behind;
    }
    gp_Trsf shift;
    if (behind > 0.0) shift.SetTranslation(dir * (-behind));

    try {
        TopoDS_Shape result;
        for (const TopoDS_Face& face : faces) {
            const TopoDS_Face start =
                behind > 0.0 ? TopoDS::Face(BRepBuilderAPI_Transform(face, shift, true).Shape()) : face;
            TopoDS_Shape piece = Sweep(start, dir, length, f.taper.value);
            if (IsEmpty(piece)) return Error{"Не удалось построить выдавливание (уклон слишком большой?)"};
            result = result.IsNull() ? piece : BRepAlgoAPI_Fuse(result, piece).Shape();
        }
        if (slanted) {
            // Keep the part on the sketch's side of the plane.
            GProp_GProps props;
            for (const TopoDS_Face& face : faces) BRepGProp::SurfaceProperties(face, props);
            const TopoDS_Face plane = BRepBuilderAPI_MakeFace(*slanted).Face();
            const TopoDS_Solid half = BRepPrimAPI_MakeHalfSpace(plane, props.CentreOfMass()).Solid();
            result = BRepAlgoAPI_Common(result, half).Shape();
            if (IsEmpty(result)) return Error{"Плоскость «до» не пересекает выдавливание"};
        }
        return Clean(result);
    } catch (const Standard_Failure& failure) {
        return Error{std::format("Ошибка выдавливания: {}", failure.GetMessageString())};
    }
}

namespace {

/// A face of a cut's tool and a point just under it, inside the tool.
struct Watched {
    TopoDS_Face face;
    gp_Pnt      under;
};

/// What a boolean gave, and for a cut what became of it.
struct Ran {
    TopoDS_Shape shape;
    bool         touched = true;   ///< a face of the first argument was changed or removed
    int          gone = 0;         ///< watched tool faces that left nothing in the result
};

/// `Op` (Fuse, Cut, Common) of `a` and `tools`. On the calling thread, as
/// every boolean (SetUpOcct in Model.cpp turns OCCT's parallel mode off).
/// `fuzzy` > 0: with that tolerance, mm — surfaces that only touch (a
/// cylinder ending on another's generatrix, a profile corner exactly on a
/// wall) are then treated as meeting; the arguments are left as they were
/// (a failed boolean changes their tolerances otherwise).
template <class Op>
Ran RunBoolean(const TopoDS_Shape& a, const TopTools_ListOfShape& tools, double fuzzy, const std::vector<Watched>* watch = nullptr) {
    TopTools_ListOfShape arguments;
    arguments.Append(a);
    Op op;
    op.SetArguments(arguments);
    op.SetTools(tools);
    if (fuzzy > 0) {
        op.SetFuzzyValue(fuzzy);
        op.SetNonDestructive(true);
    }
    op.Build();
    if (op.HasErrors()) throw Standard_Failure("the boolean operation reported errors");
    Ran ran;
    ran.shape = op.Shape();   // throws StdFail_NotDone when it failed
    if (watch) {
        ran.touched = false;
        for (TopExp_Explorer e(a, TopAbs_FACE); e.More() && !ran.touched; e.Next())
            ran.touched = op.IsDeleted(e.Current()) || !op.Modified(e.Current()).IsEmpty();
        for (const Watched& w : *watch) ran.gone += op.IsDeleted(w.face) ? 1 : 0;
    }
    return ran;
}

template <class Op>
Ran RunBoolean(const TopoDS_Shape& a, const TopoDS_Shape& b, double fuzzy = 0.0, const std::vector<Watched>* watch = nullptr) {
    TopTools_ListOfShape tools;
    tools.Append(b);
    return RunBoolean<Op>(a, tools, fuzzy, watch);
}

/// The tolerances a boolean is tried with, in turn: exact first, then the
/// touching-surfaces ones (BETA-BUGS №5, №7). Far below anything drawn on
/// purpose: 0.1 µm, then 10 µm.
constexpr double kFuzzySteps[] = {0.0, 1e-4, 1e-2};

const char* const kNothingLeft = "После операции не осталось материала";

/// What every boolean result goes through: not empty, cleaned, valid.
Expected<TopoDS_Shape> FinishBoolean(const TopoDS_Shape& raw, doc::BooleanOp op) {
    if (IsEmpty(raw)) {
        return Error{op == doc::BooleanOp::Intersect ? "Пересечение пустое: тела не перекрываются" : kNothingLeft};
    }
    const TopoDS_Shape result = Clean(raw);
    if (!BRepCheck_Analyzer(result).IsValid()) return Error{"Результат операции некорректен"};
    return result;
}

/// Whether points are in the material of a shape: rays from the point cross
/// its surface an odd number of times. Not BRepClass3d_SolidClassifier: it
/// faults inside on some points in OCCT 7.9.3 / 8 of MSYS2 (see Thread.cpp).
/// Three skew rays, the majority: one through an edge can miscount.
class MaterialProbe {
public:
    explicit MaterialProbe(const TopoDS_Shape& shape) { myRays.Load(shape, 1e-7); }
    bool Inside(const gp_Pnt& p) {
        static const gp_Dir kDirs[] = {gp_Dir(0.7, 0.2, 0.3), gp_Dir(-0.3, -0.6, 0.7), gp_Dir(0.4, 0.5, -0.75)};
        int inside = 0;
        for (int i = 0; i < 3; ++i) {
            if (i == 2 && inside != 1) break;   // the first two agree
            myRays.Perform(gp_Lin(p, kDirs[i]), 0, Precision::Infinite());
            if (myRays.IsDone() && myRays.NbPnt() % 2 == 1) ++inside;
        }
        return inside >= 2;
    }

private:
    IntCurvesFace_ShapeIntersector myRays;
};

double VolumeOf(const TopoDS_Shape& shape) {
    GProp_GProps props;
    BRepGProp::VolumeProperties(shape, props);
    return props.Mass();
}

/// A tool with more faces than this is a swept one (a «Пружина», a thread,
/// a sweep along a path): the kind whose cut can come out incomplete.
constexpr int kSweptToolFaces = 24;

int FaceCount(const TopoDS_Shape& shape) {
    int n = 0;
    for (TopExp_Explorer e(shape, TopAbs_FACE); e.More(); e.Next()) ++n;
    return n;
}

/// Faces of a cut's `tool` that must leave something in the result: those
/// with the tool's material under them inside `target`. A cut removes that
/// material, so what is left of such a face becomes a wall of the cut; a
/// face that left nothing was not cut along. Not every face when there are
/// hundreds (a coil): some fifty of them, spread over the tool.
std::vector<Watched> FacesToWatch(const TopoDS_Shape& target, const TopoDS_Shape& tool) {
    std::vector<TopoDS_Face> faces;
    for (TopExp_Explorer e(tool, TopAbs_FACE); e.More(); e.Next()) faces.push_back(TopoDS::Face(e.Current()));
    if (faces.empty()) return {};
    constexpr std::size_t kWanted = 48;
    const std::size_t count = std::min(faces.size(), kWanted);
    MaterialProbe material(target);
    std::vector<Watched> watched;
    for (std::size_t k = 0; k < count; ++k) {
        // A stride that shares no factor with the faces of one section: every
        // n-th face of a sweep is the same side of its profile.
        const TopoDS_Face& face = faces[faces.size() <= kWanted ? k : (k * 7919) % faces.size()];
        BRepGProp_Face surface(face);   // its normal looks out of the solid
        double u1, u2, v1, v2;
        surface.Bounds(u1, u2, v1, v2);
        if (!std::isfinite(u1) || !std::isfinite(u2) || !std::isfinite(v1) || !std::isfinite(v2)) continue;
        static constexpr double kAt[][2] = {{0.5, 0.5}, {0.27, 0.31}, {0.73, 0.69}};
        for (const auto& at : kAt) {
            const double u = u1 + (u2 - u1) * at[0], v = v1 + (v2 - v1) * at[1];
            if (BRepClass_FaceClassifier(face, gp_Pnt2d(u, v), 1e-7).State() != TopAbs_IN) continue;
            gp_Pnt on;
            gp_Vec normal;
            surface.Normal(u, v, on, normal);
            if (normal.Magnitude() < 1e-12) continue;
            const gp_Pnt under = on.Translated(normal.Normalized() * -0.005);
            if (material.Inside(under)) watched.push_back({face, under});
            break;
        }
    }
    return watched;
}

/// A cut that left nothing of more than this share of the faces it had to
/// cut along is not a complete cut.
constexpr double kUncutLimit = 0.2;

} // namespace

TopoDS_Shape Fuse(const TopoDS_Shape& a, const TopoDS_Shape& b) { return RunBoolean<BRepAlgoAPI_Fuse>(a, b).shape; }

Expected<TopoDS_Shape> ApplyBoolean(const TopoDS_Shape& target, const TopoDS_Shape& tool, doc::BooleanOp op, BooleanNote* note) {
    if (op == doc::BooleanOp::NewBody) return tool;
    BooleanNote local;
    BooleanNote& said = note ? *note : local;
    said = {};
    const bool cut = op == doc::BooleanOp::Cut;
    // A cut is checked by what it did, not only by the shape being valid:
    // OCCT can return a valid body that lost no material — or, with a swept
    // tool (a «Пружина», a thread), a groove cut along a part of the tool
    // only — when the tool's corners lie exactly on the body's surface
    // (BETA-BUGS №5). Whether material went: the volume. Whether all of a
    // swept tool cut: faces of the tool that had to become walls of the cut
    // (FacesToWatch). An ordinary tool is never judged by its faces: one
    // ending in a cavity of the body leaves no wall there, rightly.
    const double before = cut ? VolumeOf(target) : 0.0;
    const double noise = std::max(1e-6, before * 1e-9);
    std::vector<Watched> watch;
    if (cut && FaceCount(tool) > kSweptToolFaces) watch = FacesToWatch(target, tool);
    std::optional<TopoDS_Shape> first;   // the exact cut, kept when no other is complete either
    bool                        missed = false;   // a valid cut came back without removing anything
    bool                        firstPartial = false;
    Error                       last{"Ошибка булевой операции"};
    for (double fuzzy : kFuzzySteps) {
        said.retried = fuzzy > 0;
        try {
            Ran ran;
            switch (op) {
                case doc::BooleanOp::Join:      ran = RunBoolean<BRepAlgoAPI_Fuse>(target, tool, fuzzy); break;
                case doc::BooleanOp::Cut:       ran = RunBoolean<BRepAlgoAPI_Cut>(target, tool, fuzzy, watch.empty() ? nullptr : &watch); break;
                case doc::BooleanOp::Intersect: ran = RunBoolean<BRepAlgoAPI_Common>(target, tool, fuzzy); break;
                case doc::BooleanOp::NewBody:   break;
            }
            if (IsEmpty(ran.shape)) return FinishBoolean(ran.shape, op);   // nothing left is an answer, not a failure
            Expected<TopoDS_Shape> result = FinishBoolean(ran.shape, op);
            if (!result) {
                last = result.GetError();
                continue;   // not valid: once more, with a tolerance
            }
            if (!cut) return result;
            const bool removed = VolumeOf(*result) < before - noise;
            // One face of a few is a ray gone wrong, not a groove left uncut.
            const bool partial = removed && ran.gone >= 2 && ran.gone > kUncutLimit * static_cast<double>(watch.size());
            if (removed && !partial) return result;
            missed = missed || !removed;
            if (!first && removed) {
                first = *result;
                firstPartial = partial;
            }
        } catch (const Standard_Failure& failure) {
            last = Error{std::format("Ошибка булевой операции: {}", failure.GetMessageString())};
        }
    }
    if (first) {   // material went, but not all of it, however it was tried
        said.retried = false;
        said.partial = firstPartial;
        return *first;
    }
    // A valid cut that removed nothing at any tolerance: it missed the body.
    if (missed) {
        said.touched = false;
        return target;
    }
    return last;
}

Expected<TopoDS_Shape> ApplyBoolean(const TopoDS_Shape& target, const std::vector<TopoDS_Shape>& tools, doc::BooleanOp op) {
    if (tools.empty()) return target;
    if (op != doc::BooleanOp::Join && op != doc::BooleanOp::Cut) return Error{"Копировать можно только добавление и вырезание"};
    try {
        TopTools_ListOfShape toolList;
        for (const TopoDS_Shape& t : tools) toolList.Append(t);
        // As a single tool: exact first, then with a tolerance for surfaces
        // that only touch.
        Error last{"Ошибка булевой операции"};
        for (double fuzzy : kFuzzySteps) {
            try {
                const TopoDS_Shape raw = op == doc::BooleanOp::Join ? RunBoolean<BRepAlgoAPI_Fuse>(target, toolList, fuzzy).shape
                                                                    : RunBoolean<BRepAlgoAPI_Cut>(target, toolList, fuzzy).shape;
                Expected<TopoDS_Shape> result = FinishBoolean(raw, op);
                if (result || IsEmpty(raw)) return result;
                last = result.GetError();
            } catch (const Standard_Failure& failure) {
                last = Error{std::format("Ошибка булевой операции: {}", failure.GetMessageString())};
            }
        }
        return last;
    } catch (const Standard_Failure& failure) {
        return Error{std::format("Ошибка булевой операции: {}", failure.GetMessageString())};
    }
}

namespace {

/// Whether `edge` lies between two faces of one and the same cylinder: a
/// line drawn on it by an earlier operation, not an edge of the shape.
bool SameCylinderOnBothSides(const TopoDS_Shape& edge, const TopTools_IndexedDataMapOfShapeListOfShape& edgeFaces) {
    std::vector<TopoDS_Face> two;
    for (const TopoDS_Shape& f : edgeFaces.FindFromKey(edge))
        if (std::ranges::none_of(two, [&](const TopoDS_Face& t) { return t.IsSame(f); })) two.push_back(TopoDS::Face(f));
    if (two.size() != 2) return false;
    const BRepAdaptor_Surface a(two[0]), b(two[1]);
    if (a.GetType() != GeomAbs_Cylinder || b.GetType() != GeomAbs_Cylinder) return false;
    const gp_Cylinder ca = a.Cylinder(), cb = b.Cylinder();
    return std::abs(ca.Radius() - cb.Radius()) < 1e-6 && ca.Axis().IsParallel(cb.Axis(), 1e-6) &&
           gp_Lin(ca.Axis()).Distance(cb.Location()) < 1e-6;
}

} // namespace

std::optional<Vec3> FindNewSliver(const TopoDS_Shape& before, const TopoDS_Shape& after, const TopoDS_Shape& tool) {
    constexpr double kNarrow = 0.05;   // mm: narrower than anything printed or milled on purpose
    Bnd_Box around;
    BRepBndLib::Add(tool, around);
    if (around.IsVoid()) return std::nullopt;
    around.Enlarge(0.1);
    // Narrow faces near the tool, as points: those of `after` that `before`
    // had too are not this operation's.
    // Also a step a hair high between two flat faces looking the same way
    // (a pocket 6.15 deep under a wall at 6.12): the strip between them can
    // be merged into a larger face of the same surface, and is not narrow.
    struct Flat {
        gp_Pln  plane;
        Bnd_Box box;
        gp_Pnt  centre;
    };
    const auto narrow = [&](const TopoDS_Shape& shape) {
        std::vector<gp_Pnt> found;
        std::vector<Flat>   flats;
        for (TopExp_Explorer e(shape, TopAbs_FACE); e.More(); e.Next()) {
            Bnd_Box box;
            BRepBndLib::Add(e.Current(), box);
            if (box.IsVoid() || around.IsOut(box)) continue;
            const TopoDS_Face& face = TopoDS::Face(e.Current());
            const BRepAdaptor_Surface surface(face);
            if (surface.GetType() == GeomAbs_Plane) {
                gp_Pln plane = surface.Plane();
                if (face.Orientation() == TopAbs_REVERSED) plane.SetAxis(plane.Axis().Reversed());
                GProp_GProps props;
                BRepGProp::SurfaceProperties(face, props);
                for (const Flat& other : flats) {
                    if (!plane.Axis().Direction().IsEqual(other.plane.Axis().Direction(), 1e-6)) continue;
                    const double gap = other.plane.Distance(plane.Location());
                    if (gap < 1e-5 || gap >= kNarrow) continue;
                    Bnd_Box a = box, b = other.box;
                    a.Enlarge(kNarrow);
                    if (!a.IsOut(b)) found.push_back(props.CentreOfMass());
                }
                flats.push_back({plane, box, props.CentreOfMass()});
            }
            // A strip at most this wide fits its box this way: the box's
            // smallest side across a flat face, or area against the outline.
            GProp_GProps area, outline;
            BRepGProp::SurfaceProperties(e.Current(), area);
            if (area.Mass() <= 1e-9) continue;
            BRepGProp::LinearProperties(e.Current(), outline);
            if (outline.Mass() <= 1e-9) continue;
            const double width = 2 * area.Mass() / outline.Mass();
            if (width < kNarrow) found.push_back(area.CentreOfMass());
        }
        return found;
    };
    const std::vector<gp_Pnt> had = narrow(before);
    for (const gp_Pnt& p : narrow(after))
        if (std::ranges::none_of(had, [&](const gp_Pnt& q) { return q.Distance(p) < 1e-4; })) return Vec3{p.X(), p.Y(), p.Z()};
    return std::nullopt;
}

TopoDS_Shape Clean(const TopoDS_Shape& input) {
    // One cylinder made twice (a fillet and the extruded arc projected from
    // it) is given one surface first: with two frames of opposite hands
    // UnifySameDomain never finished, and the line between them stayed.
    const TopoDS_Shape shape = ShareCylinders(input);
    auto* unify = new ShapeUpgrade_UnifySameDomain(shape, true, true, true);
    // Curved faces next to free-form ones (a cylinder cut by a thread
    // groove), or bounded by an intersection curve of two curved surfaces (a
    // tube cut by a ring across it), are left as they are: merging them can
    // take OCCT minutes — or, for the ring, longer than anyone waits.
    TopTools_IndexedDataMapOfShapeListOfShape edgeFaces;
    TopExp::MapShapesAndAncestors(shape, TopAbs_EDGE, TopAbs_FACE, edgeFaces);
    const auto typeOf = [](const TopoDS_Shape& face) { return BRepAdaptor_Surface(TopoDS::Face(face)).GetType(); };
    const auto freeform = [&](const TopoDS_Shape& face) { return typeOf(face) > GeomAbs_Torus; };
    const auto intersection = [](const TopoDS_Shape& edge) {
        const TopoDS_Edge& e = TopoDS::Edge(edge);
        return !BRep_Tool::Degenerated(e) && BRepAdaptor_Curve(e).GetType() > GeomAbs_Parabola;
    };
    for (TopExp_Explorer f(shape, TopAbs_FACE); f.More(); f.Next()) {
        if (typeOf(f.Current()) == GeomAbs_Plane) continue;
        bool touches = freeform(f.Current());
        for (TopExp_Explorer e(f.Current(), TopAbs_EDGE); e.More() && !touches; e.Next()) {
            touches = intersection(e.Current());
            for (const TopoDS_Shape& other : edgeFaces.FindFromKey(e.Current())) touches = touches || freeform(other);
        }
        // Such a face keeps its intersection curves and its edges against
        // free-form faces — what makes merging slow. A straight or round
        // edge between two pieces of the same cylinder (a hole cut by a
        // pocket drawn from the hole's own projected arc) is let go: kept,
        // it stayed on the model as a line nothing could be rounded across.
        if (touches)
            for (TopExp_Explorer e(f.Current(), TopAbs_EDGE); e.More(); e.Next()) {
                bool slow = intersection(e.Current()) || freeform(f.Current());
                for (const TopoDS_Shape& other : edgeFaces.FindFromKey(e.Current())) slow = slow || freeform(other);
                if (slow || !SameCylinderOnBothSides(e.Current(), edgeFaces)) unify->KeepShape(e.Current());
            }
    }
    // UnifySameDomain itself faults on some shapes (OCCT 7.8: a revolve of a
    // profile made of hundreds of short lines — SIGSEGV in IntUnifyFaces).
    // Cleaning is a nicety: on a fault the shape goes on as the boolean or
    // sweep made it. The half-built object is left alone (leaked), its state
    // after the jump is not to be trusted even by its destructor.
    TopoDS_Shape result;
    if (!guard::RunGuarded([&] { unify->Build(); result = unify->Shape(); })) {
        log::Warn("Clean: OCCT fault in UnifySameDomain, the shape is left as it is");
        return input;
    }
    delete unify;
    return result;
}

} // namespace bz::kernel::occt
