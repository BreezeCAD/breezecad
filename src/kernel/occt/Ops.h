#pragma once
// The one place booleans of the geometry kernel are made (STAB-3).
//
// Every boolean of every operation goes through one pipeline:
//   build exactly → again with a touching tolerance (fuzzy) → glued → healed
// and every result is checked before it is taken: the faces the boolean
// made are valid, the volume changed the way the operation means, a swept
// tool cut along its whole length, no strip a hair wide was left. What comes
// back is a body, a code and a Russian sentence for the user; OCCT's own text
// goes to the log only. Each attempt runs under a deadline (Deadline) and a
// fault guard: no boolean hangs the program or takes it down.
//
// Outside this file no BRepAlgoAPI_* is used (tests/core/SourceRulesTests.cpp
// fails the build otherwise; Project.cpp is stage 5 and listed there).

#include "core/Expected.h"
#include "core/Math.h"
#include "document/SketchSolidFeature.h"

#include <Message_ProgressIndicator.hxx>
#include <NCollection_List.hxx>
#include <TopoDS_Shape.hxx>

#include <chrono>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

class Standard_Failure;

namespace bz::kernel::occt {

/// Stops OCCT's loops once a moment has passed: give `Start()` to an
/// algorithm's Build/Perform. Algorithms that take a Message_ProgressRange
/// look at UserBreak() between their steps (booleans, splitters, meshing,
/// pipe sweeps).
class Deadline : public Message_ProgressIndicator {
public:
    explicit Deadline(double seconds);
    explicit Deadline(std::chrono::steady_clock::time_point end) : myEnd(end) {}
    void Show(const Message_ProgressScope&, const bool) override {}
    bool UserBreak() override { return Passed(); }
    bool Passed() const { return std::chrono::steady_clock::now() > myEnd; }
    std::chrono::steady_clock::time_point End() const { return myEnd; }

private:
    std::chrono::steady_clock::time_point myEnd;
};

namespace ops {

enum class Kind { Fuse, Cut, Common };

/// What a pipeline came to. Stable numbers: they go to the log and to tests.
enum class Code : int {
    Ok = 0,         ///< exact, checked
    Retried = 1,    ///< made with a touching tolerance or glued; checked
    Healed = 2,     ///< made valid by ShapeFix; checked
    Partial = 10,   ///< a cut that removed material, but not all the tool covers (a swept tool)
    Missed = 11,    ///< a cut that reached no material: the target comes back as it was
    Empty = 12,     ///< nothing left (a cut of everything, an intersection of nothing)
    Invalid = 20,   ///< every attempt gave a body that failed the checks
    Failed = 21,    ///< OCCT reported errors or threw on every attempt
    Timeout = 22,   ///< the deadline passed
    Fault = 23,     ///< a hardware fault inside OCCT (caught by the fault guard)
};

/// A usable body came back (Ok, Retried, Healed, Partial, Missed).
inline bool Usable(Code c) { return c == Code::Ok || c == Code::Retried || c == Code::Healed || c == Code::Partial || c == Code::Missed; }

/// Seconds the whole pipeline of one boolean may take (all attempts). Below
/// the program's "not responding" watch (15 s, 30 s while work goes on),
/// far beyond any boolean of a real part (a 28-turn coil cut: 0.5 s).
inline constexpr double kBooleanSeconds = 12.0;

/// Seconds a sweep along a path or a helix may take (MakePipeShell).
inline constexpr double kSweepSeconds = 12.0;

/// The touching tolerances tried after the exact boolean, mm. Measured
/// (bz_tests -ts=measure, docs/dev/OCCT-ISSUES.md «Допуск касания»): the
/// coil profile with corners exactly on the wall (BETA-BUGS №4) is cut
/// cleanly from 1e-4 on (1e-6 and 1e-5 cut it too, but leave a strip a hair
/// wide and take 3–8 times longer); a web is eaten once the tolerance
/// reaches its thickness, and the 0.02 mm web of a thread already changes at
/// 1e-3 by 0.003 mm³ and is lost at 1e-2. So: 1e-4, then 1e-3, no further.
inline constexpr double kFuzzySteps[] = {1e-4, 1e-3};

struct Options {
    double seconds = kBooleanSeconds;   ///< deadline for all attempts together
    bool   verify = true;               ///< check the result (faces, volume, a swept cut); off for quick queries
    bool   clean = true;                ///< merge faces of one surface (Clean)
    bool   slivers = false;             ///< look for a new strip narrower than 0.05 mm (sliver)
    bool   retry = true;                ///< the fallbacks after the exact attempt
    /// The touching tolerances tried after the exact attempt (kFuzzySteps;
    /// measurements and tests give others), then glued when `glue`.
    std::vector<double> fuzzySteps{std::begin(kFuzzySteps), std::end(kFuzzySteps)};
    bool                glue = true;
    bool                exactFirst = true;   ///< false: only the fallbacks (measurements)
};

struct Result {
    TopoDS_Shape shape;   ///< the body (the target as it was for Missed); null when not Usable
    Code         code = Code::Failed;
    std::string  message;   ///< Russian, for the user: an error, or a warning for Partial / Missed / a sliver; empty when all is well
    double       fuzzy = 0.0;   ///< the touching tolerance the result was made with
    bool         glued = false;
    bool         healed = false;
    double       before = 0.0, after = 0.0;   ///< volumes of the target and of the result, mm³ (when verified)
    std::optional<Vec3> sliver;               ///< where a new strip a hair wide is (Options::slivers)
    double       ms = 0.0;                    ///< time of the whole pipeline

    bool Usable() const { return ops::Usable(code); }
    explicit operator bool() const { return Usable(); }
};

/// `kind` of `target` with `tools` (several tools act as one).
Result Boolean(Kind kind, const TopoDS_Shape& target, const NCollection_List<TopoDS_Shape>& tools, const Options& options = {});
Result Boolean(Kind kind, const TopoDS_Shape& target, const TopoDS_Shape& tool, const Options& options = {});
Result Boolean(Kind kind, const TopoDS_Shape& target, const std::vector<TopoDS_Shape>& tools, const Options& options = {});

/// Shorthands that throw nothing and give a body or an Error with Russian text.
Expected<TopoDS_Shape> Fuse(const TopoDS_Shape& a, const TopoDS_Shape& b, const Options& options = {});
Expected<TopoDS_Shape> Cut(const TopoDS_Shape& a, const TopoDS_Shape& b, const Options& options = {});
Expected<TopoDS_Shape> Common(const TopoDS_Shape& a, const TopoDS_Shape& b, const Options& options = {});

/// Options for a quick query (does it overlap, how much changed): no checks,
/// no cleaning, no fallbacks, a short deadline.
Options Quick(double seconds = 4.0);

/// Volume of the material `a` and `b` share, mm³ (0 when they do not overlap
/// or it could not be computed in time).
double SharedVolume(const TopoDS_Shape& a, const TopoDS_Shape& b, double seconds = 4.0);

/// `arguments` split by `tools` (BRepAlgoAPI_Splitter), under a deadline.
/// Error with `timeoutText` when the deadline passed.
Expected<TopoDS_Shape> Split(const NCollection_List<TopoDS_Shape>& arguments, const NCollection_List<TopoDS_Shape>& tools, double seconds,
                             const std::string& timeoutText);

/// A pipeline that gave no body, thrown out of code that builds a tool in
/// several steps; `what()` is the Russian text for the user.
class Failed : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// The body, or throws Failed with the error's text.
TopoDS_Shape OrThrow(Expected<TopoDS_Shape> result);

/// Options for building a tool from pieces (holes of a profile, regions of a
/// sketch, the head of a hole): checked, not cleaned.
Options Tool();

/// Triangulates `shape` for showing (BRepMesh_IncrementalMesh, on the calling
/// thread) under a deadline. False when the deadline passed.
bool Triangulate(const TopoDS_Shape& shape, double deflection, double angle = 0.5, bool relative = false, double seconds = 8.0);

/// The Russian name of an operation kind, for messages ("объединение"…).
std::string_view NameOf(Kind kind);

/// The text of an OCCT exception (for the log): GetMessageString up to 7.9,
/// what() from 8.0. Never null.
const char* TextOf(const Standard_Failure& failure);

/// An exception from OCCT as an Error for the user: `what` ("Ошибка пружины")
/// and a sentence that the details are in the log; OCCT's text goes to the log.
Error FromFailure(const Standard_Failure& failure, std::string_view what);

/// The warning for a strip a hair wide left by an operation (FindNewSliver).
std::string SliverMessage(const Vec3& at);

} // namespace ops

/// What a boolean was like, besides its result (CombineSolid reads it).
struct BooleanNote {
    bool        touched = true;    ///< a cut: the tool reached the target's material (false — the target came back as it was)
    bool        partial = false;   ///< a cut: material is left where the tool is, however it was tried
    bool        retried = false;   ///< made with a tolerance, glued or healed: the exact one failed or was not complete
    ops::Code   code = ops::Code::Ok;
    std::string warning;           ///< Russian text of a Partial result
    std::optional<Vec3> sliver;    ///< a new strip a hair wide (when asked)
};

/// Applies a boolean of `tool` to `target` (Model's operations). NewBody
/// gives the tool back. A result that fails the checks is tried again
/// (ops::Boolean); a cut still incomplete after that is returned with
/// `note->partial`, a cut that missed with `note->touched == false`.
Expected<TopoDS_Shape> ApplyBoolean(const TopoDS_Shape& target, const TopoDS_Shape& tool, doc::BooleanOp op,
                                    BooleanNote* note = nullptr, bool slivers = false);
/// Joins or cuts several tools at once (they may overlap each other), as
/// patterns need. NewBody and Intersect are not accepted here.
Expected<TopoDS_Shape> ApplyBoolean(const TopoDS_Shape& target, const std::vector<TopoDS_Shape>& tools, doc::BooleanOp op,
                                    BooleanNote* note = nullptr, bool slivers = false);

/// Where a boolean of `tool` turned `before` into `after` with a new face
/// narrower than 0.05 mm (two surfaces that almost, but not quite,
/// coincide: a pocket 6.15 deep under a wall at 6.12): such a strip is seen
/// as a line on the model and stops fillets. None when there is none.
std::optional<Vec3> FindNewSliver(const TopoDS_Shape& before, const TopoDS_Shape& after, const TopoDS_Shape& tool);

/// Merges coplanar faces and collinear edges left by booleans, so the model
/// keeps a clean topology (one face per flat side).
TopoDS_Shape Clean(const TopoDS_Shape& shape);

} // namespace bz::kernel::occt
