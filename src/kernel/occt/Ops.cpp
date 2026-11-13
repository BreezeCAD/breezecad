#include "kernel/occt/Ops.h"
#include "kernel/occt/ShareSurfaces.h"

#include "core/FaultGuard.h"
#include "core/Log.h"
#include "document/SketchSolidFeature.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepAlgoAPI_Splitter.hxx>
#include <BRepBndLib.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepClass_FaceClassifier.hxx>
#include <BRepGProp.hxx>
#include <BRepGProp_Face.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <IMeshTools_Parameters.hxx>
#include <BRepTools.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <IntCurvesFace_ShapeIntersector.hxx>
#include <Precision.hxx>
#include <ShapeFix_Shape.hxx>
#include <ShapeUpgrade_UnifySameDomain.hxx>
#include <Standard_Failure.hxx>
#include <Standard_Version.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <gp_Cylinder.hxx>
#include <gp_Lin.hxx>
#include <gp_Pln.hxx>
#include <gp_Pnt2d.hxx>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <memory>
#include <sstream>

namespace bz::kernel::occt {

Deadline::Deadline(double seconds)
    : myEnd(std::chrono::steady_clock::now() +
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(seconds))) {}

namespace {

using Clock = std::chrono::steady_clock;

bool IsEmpty(const TopoDS_Shape& shape) {
    if (shape.IsNull()) return true;
    TopExp_Explorer it(shape, TopAbs_SOLID);
    return !it.More();
}

double VolumeOf(const TopoDS_Shape& shape) {
    GProp_GProps props;
    BRepGProp::VolumeProperties(shape, props);
    return props.Mass();
}

int FaceCount(const TopoDS_Shape& shape) {
    int n = 0;
    for (TopExp_Explorer e(shape, TopAbs_FACE); e.More(); e.Next()) ++n;
    return n;
}

TopoDS_Shape Compound(const TopoDS_Shape& a, const NCollection_List<TopoDS_Shape>& tools) {
    TopoDS_Compound all;
    BRep_Builder builder;
    builder.MakeCompound(all);
    if (!a.IsNull()) builder.Add(all, a);
    for (const TopoDS_Shape& t : tools) builder.Add(all, t);
    return all;
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

/// A tool with more faces than this is a swept one (a «Пружина», a thread,
/// a sweep along a path): the kind whose cut can come out incomplete.
constexpr int kSweptToolFaces = 24;

/// Faces of a cut's `tool` that must leave something in the result: those
/// with the tool's material under them inside `target`. A cut removes that
/// material, so what is left of such a face becomes a wall of the cut; a
/// face that left nothing was not cut along. Not every face when there are
/// hundreds (a coil): some fifty of them, spread over the tool.
std::vector<TopoDS_Face> FacesToWatch(const TopoDS_Shape& target, const TopoDS_Shape& tool) {
    std::vector<TopoDS_Face> faces;
    for (TopExp_Explorer e(tool, TopAbs_FACE); e.More(); e.Next()) faces.push_back(TopoDS::Face(e.Current()));
    if (faces.empty()) return {};
    constexpr std::size_t kWanted = 48;
    const std::size_t count = std::min(faces.size(), kWanted);
    MaterialProbe material(target);
    std::vector<TopoDS_Face> watched;
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
            if (material.Inside(under)) watched.push_back(face);
            break;
        }
    }
    return watched;
}

/// The volume of a tool that has none (a half-space): no bound.
constexpr double kUnbounded = 1e300;

/// A cut that left nothing of more than this share of the faces it had to
/// cut along is not a complete cut.
constexpr double kUncutLimit = 0.2;

/// One way a boolean is tried.
struct Attempt {
    double fuzzy = 0.0;
    bool   glue = false;
};

/// What one attempt gave.
struct Built {
    TopoDS_Shape shape;
    bool         timeout = false;
    bool         errors = false;   ///< OCCT reported errors or threw
    bool         fault = false;    ///< a hardware fault inside
    int          gone = 0;         ///< watched tool faces that left nothing in the result
    std::string  why;              ///< for the log
};

std::unique_ptr<BRepAlgoAPI_BooleanOperation> MakeOp(ops::Kind kind) {
    switch (kind) {
        case ops::Kind::Fuse: return std::make_unique<BRepAlgoAPI_Fuse>();
        case ops::Kind::Cut: return std::make_unique<BRepAlgoAPI_Cut>();
        case ops::Kind::Common: return std::make_unique<BRepAlgoAPI_Common>();
    }
    return nullptr;
}

/// Writes the arguments of a boolean that faulted, when BZ_OCCT_DUMP names a
/// folder (developers only: the program never sends anything anywhere) —
/// the minimal .brep for tests/data/occt (docs/dev/OCCT-ISSUES.md).
void DumpFault(const TopoDS_Shape& a, const NCollection_List<TopoDS_Shape>& tools, std::string_view what) {
    const char* dir = std::getenv("BZ_OCCT_DUMP");
    if (!dir || !*dir) return;
    static std::atomic<int> counter{0};
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::filesystem::path file = std::filesystem::path(dir) / std::format("fault-{}-{}.brep", what, ++counter);
    if (BRepTools::Write(Compound(a, tools), file.string().c_str())) log::Warn("ops: arguments written to {}", file.string());
}

Built RunOnce(ops::Kind kind, const TopoDS_Shape& a, const NCollection_List<TopoDS_Shape>& tools, const Attempt& at, Clock::time_point end,
              const std::vector<TopoDS_Face>& watch) {
    Built out;
    TopoDS_Shape shape;
    int gone = 0;
    bool errors = false, timeout = false;
    std::string why;
    std::string fault;
    // The operation lives on the heap: a fault jumps out of the work below
    // and leaves it, half built, to leak rather than to be destroyed.
    BRepAlgoAPI_BooleanOperation* op = MakeOp(kind).release();
    Handle(Deadline) deadline = new Deadline(end);
    const bool survived = guard::RunGuarded(
        [&] {
            try {
                NCollection_List<TopoDS_Shape> arguments;
                arguments.Append(a);
                op->SetArguments(arguments);
                op->SetTools(tools);
                op->SetRunParallel(false);   // as every boolean (SetUpOcct in Model.cpp)
                if (at.fuzzy > 0) op->SetFuzzyValue(at.fuzzy);
                if (at.glue) op->SetGlue(BOPAlgo_GlueShift);
                // No boolean changes its arguments (OCCT may raise their
                // tolerances otherwise): the next attempt, the body kept for
                // an earlier point of the tree and a cached tool (a coil, a
                // thread: ToolCache.h) go on as they were.
                op->SetNonDestructive(true);
                op->Build(Message_ProgressRange(deadline->Start()));
                if (deadline->Passed()) {
                    timeout = true;
                    return;
                }
                if (op->HasErrors() || !op->IsDone()) {
                    std::ostringstream s;
                    op->DumpErrors(s);
                    why = s.str();
                    errors = true;
                    return;
                }
                shape = op->Shape();
                for (const TopoDS_Face& f : watch) gone += op->IsDeleted(f) ? 1 : 0;
            } catch (const Standard_Failure& failure) {
                errors = true;
                why = ops::TextOf(failure);
            }
        },
        &fault);
    if (!survived) {
        out.fault = true;
        out.why = fault;
        DumpFault(a, tools, kind == ops::Kind::Cut ? "cut" : kind == ops::Kind::Fuse ? "fuse" : "common");
        return out;   // `op` leaks on purpose
    }
    delete op;
    out.shape = shape;
    out.gone = gone;
    out.errors = errors;
    out.timeout = timeout;
    out.why = why;
    return out;
}

/// The checks of a result.
struct Verdict {
    TopoDS_Shape shape;        ///< cleaned
    bool         empty = false;
    bool         valid = false;
    bool         removed = true;   ///< a cut took material away
    bool         partial = false;  ///< a cut along part of a swept tool only
    double       after = 0.0;
    std::string  why;          ///< for the log
};

/// Faces of `shape` that none of the inputs had: what the boolean made.
std::vector<TopoDS_Face> NewFaces(const TopoDS_Shape& shape, const TopTools_IndexedMapOfShape& inputFaces) {
    std::vector<TopoDS_Face> out;
    for (TopExp_Explorer e(shape, TopAbs_FACE); e.More(); e.Next())
        if (!inputFaces.Contains(e.Current())) out.push_back(TopoDS::Face(e.Current()));
    return out;
}

/// Whether the faces a boolean made are valid and closed up with their
/// neighbours: each face by BRepCheck, each of its edges between exactly two
/// faces (a seam within one). Not BRepCheck of the whole body: on a body
/// with a coil cut into it (hundreds of faces) that took longer than the
/// boolean itself (BETA-BUGS №7), and the faces the boolean did not touch
/// were checked before.
bool ChangedFacesValid(const TopoDS_Shape& shape, const std::vector<TopoDS_Face>& made, std::string& why) {
    TopTools_IndexedDataMapOfShapeListOfShape edgeFaces;
    TopExp::MapShapesAndAncestors(shape, TopAbs_EDGE, TopAbs_FACE, edgeFaces);
    for (const TopoDS_Face& face : made) {
        if (!BRepCheck_Analyzer(face).IsValid()) {
            why = "a face made by the boolean is not valid";
            return false;
        }
        for (TopExp_Explorer e(face, TopAbs_EDGE); e.More(); e.Next()) {
            const TopoDS_Edge& edge = TopoDS::Edge(e.Current());
            if (BRep_Tool::Degenerated(edge)) continue;
            const int i = edgeFaces.FindIndex(edge);
            if (i == 0) continue;
            std::vector<TopoDS_Shape> distinct;
            for (const TopoDS_Shape& f : edgeFaces(i))
                if (std::ranges::none_of(distinct, [&](const TopoDS_Shape& d) { return d.IsSame(f); })) distinct.push_back(f);
            // Two faces; four where two solids of the result touch along the
            // edge (a non-manifold edge is valid). An odd count is a hole.
            if (distinct.size() % 2 == 0 && !distinct.empty()) continue;
            if (distinct.size() == 1 && BRep_Tool::IsClosed(edge, TopoDS::Face(distinct.front()))) continue;   // a seam
            why = std::format("an edge of a new face has {} faces", distinct.size());
            return false;
        }
    }
    return true;
}

Verdict Verify(ops::Kind kind, const TopTools_IndexedMapOfShape& inputFaces, double before, double toolVolume, const TopoDS_Shape& raw,
               int gone, std::size_t watched, const ops::Options& o) {
    Verdict v;
    if (IsEmpty(raw)) {
        v.empty = true;
        return v;
    }
    v.shape = o.clean ? Clean(raw) : raw;
    // Each solid with material: an inside-out one has a negative volume.
    for (TopExp_Explorer s(v.shape, TopAbs_SOLID); s.More(); s.Next())
        if (VolumeOf(s.Current()) <= 0) {
            v.why = "a solid of the result is inside out";
            return v;
        }
    if (!ChangedFacesValid(v.shape, NewFaces(v.shape, inputFaces), v.why)) {
        // An odd count of faces at an edge is no proof of a hole: tools that
        // only touch each other (a pattern of holes typed closer and closer)
        // meet that way in a body BRepCheck finds valid — and so did the
        // pipeline before STAB-3. The full check decides then.
        if (v.why.starts_with("an edge") && BRepCheck_Analyzer(v.shape).IsValid()) {
            log::Info("ops: {}; the whole body is valid, taken", v.why);
            v.why.clear();
        } else {
            return v;
        }
    }
    // The volume changed the way the operation means.
    v.after = VolumeOf(v.shape);
    const double noise = std::max(1e-6, before * 1e-9);
    const double slack = std::max(1e-5, (before + std::min(toolVolume, before * 1e6)) * 1e-4);
    switch (kind) {
        case ops::Kind::Cut:
            v.removed = v.after < before - noise;
            if (v.after > before + slack || v.after < before - toolVolume - slack) {
                v.why = std::format("cut volume {:.6g} out of [{:.6g}, {:.6g}]", v.after, before - toolVolume, before);
                return v;
            }
            v.partial = v.removed && gone >= 2 && gone > kUncutLimit * static_cast<double>(watched);
            break;
        case ops::Kind::Fuse:
            if (v.after < before - slack || v.after > before + toolVolume + slack) {
                v.why = std::format("fuse volume {:.6g} out of [{:.6g}, {:.6g}]", v.after, before, before + toolVolume);
                return v;
            }
            break;
        case ops::Kind::Common:
            if (v.after > std::min(before, toolVolume) + slack) {
                v.why = std::format("common volume {:.6g} above {:.6g}", v.after, std::min(before, toolVolume));
                return v;
            }
            break;
    }
    v.valid = true;
    return v;
}

ops::Result Failure(ops::Code code, ops::Kind kind, const ops::Options& o) {
    ops::Result r;
    r.code = code;
    switch (code) {
        case ops::Code::Timeout:
            r.message = std::format("Операция «{}» не уложилась в {:.0f} с и остановлена — упростите её: меньше витков, граней или рёбер",
                                    ops::NameOf(kind), o.seconds);
            break;
        case ops::Code::Fault:
            r.message = "Сбой внутри геометрического ядра — операция пропущена, остальная модель цела. Измените размеры на "
                        "0,01–0,05 мм или упростите профиль";
            break;
        case ops::Code::Invalid:
            r.message = "Результат операции некорректен: ядро не построило правильное тело даже с допуском касания — "
                        "сдвиньте профиль на 0,01–0,05 мм или измените размеры";
            break;
        default:
            r.code = ops::Code::Failed;
            r.message = std::format("Ядро не смогло выполнить {} — сдвиньте профиль на 0,01–0,05 мм или измените размеры",
                                    ops::NameOf(kind));
            break;
    }
    return r;
}

const char* const kNothingLeft = "После операции не осталось материала";
const char* const kPartial = "Вырез получился частичным: профиль касается поверхности тела — сдвиньте его на 0,01–0,05 мм внутрь или наружу";

} // namespace

namespace ops {

std::string_view NameOf(Kind kind) {
    switch (kind) {
        case Kind::Fuse: return "объединение";
        case Kind::Cut: return "вырез";
        case Kind::Common: return "пересечение";
    }
    return "операцию";
}

std::string SliverMessage(const Vec3& at) {
    return std::format("Операция оставила полоску тоньше 0,05 мм у точки ({:.2f}; {:.2f}; {:.2f}): две поверхности почти совпадают, "
                       "но не точно. Такая полоска видна линией и не даёт скруглить рёбра рядом — задайте эти размеры одним выражением "
                       "(совпадут точно) или разнесите их хотя бы на 0,05 мм",
                       at.x, at.y, at.z);
}

const char* TextOf(const Standard_Failure& failure) {
#if OCC_VERSION_HEX >= 0x080000
    const char* text = failure.what();
#else
    const char* text = failure.GetMessageString();
#endif
    return text ? text : "";
}

Error FromFailure(const Standard_Failure& failure, std::string_view what) {
    log::Warn("{}: OCCT: {}", what, ops::TextOf(failure));
    return Error{std::format("{}: геометрическое ядро не справилось — измените размеры или упростите профиль", what)};
}

TopoDS_Shape OrThrow(Expected<TopoDS_Shape> result) {
    if (!result) throw Failed(result.GetError().message);
    return *result;
}

bool Triangulate(const TopoDS_Shape& shape, double deflection, double angle, bool relative, double seconds) {
    IMeshTools_Parameters p;
    p.Deflection = deflection;
    p.Angle = angle;
    p.Relative = relative;
    p.InParallel = false;
    Handle(Deadline) deadline = new Deadline(seconds);
    BRepMesh_IncrementalMesh mesh(shape, p, Message_ProgressRange(deadline->Start()));
    return !deadline->Passed();
}

Options Tool() {
    Options o;
    o.clean = false;
    return o;
}

Options Quick(double seconds) {
    Options o;
    o.seconds = seconds;
    o.verify = false;
    o.clean = false;
    o.retry = false;
    return o;
}

Result Boolean(Kind kind, const TopoDS_Shape& target, const NCollection_List<TopoDS_Shape>& tools, const Options& o) {
    const auto started = Clock::now();
    const Clock::time_point end = started + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(o.seconds));
    const auto finish = [&](Result r, const char* how) {
        r.ms = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
        if ((r.code != Code::Ok && !(r.code == Code::Empty && !o.verify)) || r.ms > 1000)
            log::Info("ops: {} code={} fuzzy={:g} glued={} healed={} {:.0f} ms ({})", NameOf(kind), static_cast<int>(r.code), r.fuzzy,
                      r.glued, r.healed, r.ms, how);
        return r;
    };
    if (target.IsNull() || tools.IsEmpty()) {
        Result r;
        r.code = Code::Failed;
        r.message = std::format("Не удалось выполнить {}: нет тела", NameOf(kind));
        return finish(r, "no arguments");
    }

    // What the checks compare against, measured once.
    double before = 0, toolVolume = 0;
    TopTools_IndexedMapOfShape inputFaces;
    std::vector<TopoDS_Face> watch;
    if (o.verify) {
        std::string fault;
        const bool measured = guard::RunGuarded(
            [&] {
                try {
                    before = VolumeOf(target);
                    // A tool without a finite volume (a half-space, a sheet)
                    // bounds nothing: its volume is taken as unlimited.
                    for (const TopoDS_Shape& t : tools) {
                        const double v = VolumeOf(t);
                        toolVolume += std::isfinite(v) && v > 1e-9 ? v : kUnbounded;
                    }
                    TopExp::MapShapes(target, TopAbs_FACE, inputFaces);
                    for (const TopoDS_Shape& t : tools) TopExp::MapShapes(t, TopAbs_FACE, inputFaces);
                    // A cut is checked by what it did, not only by the shape
                    // being valid: OCCT can return a valid body that lost no
                    // material — or, with a swept tool (a «Пружина», a thread),
                    // a groove cut along a part of the tool only — when the
                    // tool's corners lie exactly on the body's surface
                    // (BETA-BUGS №4). An ordinary tool is never judged by its
                    // faces: one ending in a cavity of the body leaves no wall there.
                    if (kind == Kind::Cut && tools.Size() == 1 && FaceCount(tools.First()) > kSweptToolFaces)
                        watch = FacesToWatch(target, tools.First());
                } catch (const Standard_Failure& failure) {
                    log::Warn("ops: measuring the arguments: {}", ops::TextOf(failure));
                }
            },
            &fault);
        if (!measured) {
            DumpFault(target, tools, "measure");
            log::Warn("ops: fault while measuring the arguments: {}", fault);
            return finish(Failure(Code::Fault, kind, o), "fault measuring");
        }
    }

    std::vector<Attempt> attempts;
    if (o.exactFirst) attempts.push_back({0.0, false});
    if (o.retry) {
        for (double f : o.fuzzySteps) attempts.push_back({f, false});
        if (o.glue) attempts.push_back({0.0, true});
    }
    // For new strips a hair wide: all the tools as one shape.
    TopoDS_Shape toolShape;
    if (o.slivers) toolShape = tools.Size() == 1 ? tools.First() : Compound({}, tools);
    const auto sliverOf = [&](const TopoDS_Shape& shape) -> std::optional<Vec3> {
        if (!o.slivers) return std::nullopt;
        std::optional<Vec3> at;
        std::string fault;
        guard::RunGuarded([&] {
            try {
                at = FindNewSliver(target, shape, toolShape);
            } catch (const Standard_Failure&) {
                at.reset();
            }
        }, &fault);
        return at;
    };
    std::optional<Result> slivered;   // a complete result with a new strip, made with a tolerance

    std::optional<Result> partial;   // material went, but not all of it
    bool missed = false;             // a valid cut came back without removing anything
    TopoDS_Shape invalid;            // the first body that failed the checks: healed last
    double invalidFuzzy = 0;
    Code last = Code::Failed;
    for (const Attempt& at : attempts) {
        if (Clock::now() > end) return finish(Failure(Code::Timeout, kind, o), "deadline before an attempt");
        const Built built = RunOnce(kind, target, tools, at, end, watch);
        const char* how = at.glue ? "glued" : at.fuzzy > 0 ? "fuzzy" : "exact";
        if (built.timeout) return finish(Failure(Code::Timeout, kind, o), how);
        if (built.fault) {
            log::Warn("ops: {} faulted ({}): {}", NameOf(kind), how, built.why);
            last = Code::Fault;
            continue;   // another tolerance takes another way through OCCT
        }
        if (built.errors) {
            log::Info("ops: {} failed ({}): {}", NameOf(kind), how, built.why);
            if (last != Code::Fault) last = Code::Failed;
            continue;
        }
        Result r;
        r.fuzzy = at.fuzzy;
        r.glued = at.glue;
        r.before = before;
        r.code = at.fuzzy > 0 || at.glue ? Code::Retried : Code::Ok;
        if (!o.verify) {
            // Unchecked results may be faces or edges (a section by a sheet): empty is nothing at all.
            if (built.shape.IsNull() || !TopExp_Explorer(built.shape, TopAbs_EDGE).More()) {
                r.code = Code::Empty;
                return finish(r, how);
            }
            r.shape = o.clean ? Clean(built.shape) : built.shape;
            return finish(r, how);
        }
        Verdict v;
        std::string fault;
        if (!guard::RunGuarded([&] {
                try {
                    v = Verify(kind, inputFaces, before, toolVolume, built.shape, built.gone, watch.size(), o);
                } catch (const Standard_Failure& failure) {
                    v.why = ops::TextOf(failure);
                }
            }, &fault)) {
            log::Warn("ops: fault while checking a result: {}", fault);
            last = Code::Fault;
            continue;
        }
        if (v.empty) {   // nothing left is an answer, not a failure
            r.code = Code::Empty;
            r.message = kind == Kind::Common ? "Пересечение пустое: тела не перекрываются" : kNothingLeft;
            return finish(r, how);
        }
        if (!v.valid) {
            log::Info("ops: {} result rejected ({}): {}", NameOf(kind), how, v.why);
            if (invalid.IsNull()) {
                invalid = built.shape;
                invalidFuzzy = at.fuzzy;
            }
            if (last != Code::Fault) last = Code::Invalid;
            continue;   // not valid: once more, another way
        }
        r.shape = v.shape;
        r.after = v.after;
        if (kind != Kind::Cut || (v.removed && !v.partial)) {
            // A strip a hair wide: the user's own sizes when the exact
            // boolean left it (the warning says which); when a tolerance
            // did, the next tolerance may not — kept in case none does better.
            r.sliver = sliverOf(r.shape);
            if (!r.sliver) return finish(r, how);
            r.message = ops::SliverMessage(*r.sliver);
            if (r.code == Code::Ok) return finish(r, how);
            if (!slivered) slivered = r;
            continue;
        }
        missed = missed || !v.removed;
        if (!partial && v.removed) {
            r.code = Code::Partial;
            r.message = kPartial;
            partial = r;
        }
    }
    if (slivered) return finish(*slivered, "with a strip");
    if (partial) return finish(*partial, "partial");
    if (missed) {   // a valid cut that removed nothing at any tolerance: it missed the body
        Result r;
        r.code = Code::Missed;
        r.shape = target;
        r.before = r.after = before;
        r.message = "Вырез не задел тело — ничего не изменилось";
        return finish(r, "missed");
    }
    // The last way: the body that failed the checks, healed.
    if (!invalid.IsNull() && o.retry && Clock::now() < end) {
        Result r;
        std::string fault;
        Verdict v;
        const bool survived = guard::RunGuarded([&] {
            try {
                ShapeFix_Shape fix(invalid);
                Handle(Deadline) deadline = new Deadline(end);
                fix.Perform(Message_ProgressRange(deadline->Start()));
                if (deadline->Passed()) return;
                v = Verify(kind, inputFaces, before, toolVolume, fix.Shape(), 0, 0, o);
            } catch (const Standard_Failure& failure) {
                v.why = ops::TextOf(failure);
            }
        }, &fault);
        if (survived && v.valid && (kind != Kind::Cut || v.removed)) {
            r.code = Code::Healed;
            r.healed = true;
            r.fuzzy = invalidFuzzy;
            r.shape = v.shape;
            r.before = before;
            r.after = v.after;
            return finish(r, "healed");
        }
        log::Info("ops: healing did not help: {}", survived ? v.why : fault);
    }
    return finish(Failure(last, kind, o), "all attempts");
}

Result Boolean(Kind kind, const TopoDS_Shape& target, const TopoDS_Shape& tool, const Options& o) {
    NCollection_List<TopoDS_Shape> tools;
    if (!tool.IsNull()) tools.Append(tool);
    return Boolean(kind, target, tools, o);
}

Result Boolean(Kind kind, const TopoDS_Shape& target, const std::vector<TopoDS_Shape>& tools, const Options& o) {
    NCollection_List<TopoDS_Shape> list;
    for (const TopoDS_Shape& t : tools)
        if (!t.IsNull()) list.Append(t);
    return Boolean(kind, target, list, o);
}

namespace {
Expected<TopoDS_Shape> AsExpected(Result r) {
    if (r.code == Code::Empty) return Error{r.message.empty() ? std::string(kNothingLeft) : r.message};
    if (!r.Usable()) return Error{r.message};
    return r.shape;
}
} // namespace

Expected<TopoDS_Shape> Fuse(const TopoDS_Shape& a, const TopoDS_Shape& b, const Options& o) { return AsExpected(Boolean(Kind::Fuse, a, b, o)); }
Expected<TopoDS_Shape> Cut(const TopoDS_Shape& a, const TopoDS_Shape& b, const Options& o) { return AsExpected(Boolean(Kind::Cut, a, b, o)); }
Expected<TopoDS_Shape> Common(const TopoDS_Shape& a, const TopoDS_Shape& b, const Options& o) {
    return AsExpected(Boolean(Kind::Common, a, b, o));
}

double SharedVolume(const TopoDS_Shape& a, const TopoDS_Shape& b, double seconds) {
    const Result r = Boolean(Kind::Common, a, b, Quick(seconds));
    if (!r.Usable() || r.shape.IsNull()) return 0.0;
    double v = 0;
    std::string fault;
    if (!guard::RunGuarded([&] { v = VolumeOf(r.shape); }, &fault)) return 0.0;
    return v;
}

Expected<TopoDS_Shape> Split(const NCollection_List<TopoDS_Shape>& arguments, const NCollection_List<TopoDS_Shape>& tools, double seconds,
                             const std::string& timeoutText) {
    TopoDS_Shape shape;
    bool timeout = false, failed = false;
    std::string why, fault;
    auto* splitter = new BRepAlgoAPI_Splitter();
    Handle(Deadline) deadline = new Deadline(seconds);
    const auto started = Clock::now();
    const bool survived = guard::RunGuarded(
        [&] {
            try {
                splitter->SetArguments(arguments);
                splitter->SetTools(tools);
                splitter->SetRunParallel(false);
                splitter->Build(Message_ProgressRange(deadline->Start()));
                if (deadline->Passed() || Clock::now() - started > std::chrono::duration<double>(seconds)) {
                    timeout = true;
                    return;
                }
                if (!splitter->IsDone() || splitter->HasErrors()) {
                    failed = true;
                    return;
                }
                shape = splitter->Shape();
            } catch (const Standard_Failure& failure) {
                failed = true;
                why = ops::TextOf(failure);
            }
        },
        &fault);
    if (!survived) {
        log::Warn("ops: fault in a splitter: {}", fault);
        DumpFault(Compound({}, arguments), tools, "split");
        return Error{"Сбой внутри геометрического ядра при разбиении — измените размеры или упростите эскиз"};
    }
    delete splitter;
    if (timeout) return Error{timeoutText};
    if (failed) {
        if (!why.empty()) log::Info("ops: splitter: {}", why);
        return Error{"Геометрическое ядро не смогло разбить фигуру"};
    }
    return shape;
}

} // namespace ops

namespace {

BooleanNote NoteOf(const ops::Result& r) {
    BooleanNote n;
    n.code = r.code;
    n.touched = r.code != ops::Code::Missed;
    n.partial = r.code == ops::Code::Partial;
    n.retried = r.code == ops::Code::Retried || r.code == ops::Code::Healed;
    if (n.partial) n.warning = r.message;
    n.sliver = r.sliver;
    return n;
}

ops::Kind KindOf(doc::BooleanOp op) {
    switch (op) {
        case doc::BooleanOp::Cut: return ops::Kind::Cut;
        case doc::BooleanOp::Intersect: return ops::Kind::Common;
        default: return ops::Kind::Fuse;
    }
}

Expected<TopoDS_Shape> Finish(const ops::Result& r, BooleanNote* note) {
    if (note) *note = NoteOf(r);
    if (r.code == ops::Code::Empty) return Error{r.message};
    if (!r.Usable()) return Error{r.message};
    return r.shape;
}

} // namespace

Expected<TopoDS_Shape> ApplyBoolean(const TopoDS_Shape& target, const TopoDS_Shape& tool, doc::BooleanOp op, BooleanNote* note,
                                    bool slivers) {
    if (note) *note = {};
    if (op == doc::BooleanOp::NewBody) return tool;
    ops::Options o;
    o.slivers = slivers;
    return Finish(ops::Boolean(KindOf(op), target, tool, o), note);
}

Expected<TopoDS_Shape> ApplyBoolean(const TopoDS_Shape& target, const std::vector<TopoDS_Shape>& tools, doc::BooleanOp op,
                                    BooleanNote* note, bool slivers) {
    if (note) *note = {};
    if (tools.empty()) return target;
    if (op != doc::BooleanOp::Join && op != doc::BooleanOp::Cut) return Error{"Копировать можно только добавление и вырезание"};
    ops::Options o;
    o.slivers = slivers;
    return Finish(ops::Boolean(KindOf(op), target, tools, o), note);
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
    // Faces of a real part are "one plane" only up to the sketch solver's
    // residual and the transforms of a part drawn in an assembly. The
    // default angular tolerance (1e-12 rad) left such faces apart, with a
    // seam no fillet could cross (a tower stacked from projected sketches:
    // 6e-12 rad). 1e-9 rad takes those in; faces further apart (1e-7 mm over
    // 60 mm) merged into a face the fillet then failed on, so they stay
    // apart as before. The linear tolerance stays OCCT's 1e-7 mm.
    unify->SetAngularTolerance(1e-9);
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
