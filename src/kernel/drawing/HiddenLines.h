#pragma once
// The lines of a drawing view (ADR-0012, step 3): the model's edges and
// outlines seen from a direction, visible and hidden apart (OCCT's exact
// hidden line removal, HLRBRep_Algo), and the centre lines of its round
// holes and shafts. All in the view's plane at 1:1, the model's origin at
// (0, 0); the drawing scales and places them (drawing/SheetLayout.h).

#include "core/Expected.h"
#include "core/Math.h"
#include "document/EdgeReference.h"

#include <memory>
#include <optional>
#include <vector>

namespace bz::kernel {

class Model;

struct ProjectedLines {
    std::vector<std::vector<Vec2>> visible, hidden;
    struct Mark {
        Vec2   center;
        double radius = 0;
    };
    struct Axis {
        Vec2 a, b;
    };
    std::vector<Mark> marks;   ///< round faces seen end on
    std::vector<Axis> axes;    ///< round faces seen from the side
    /// A section (ADR-0012, step 6): the faces on the cutting plane, each as
    /// its closed loops (outer and holes) — hatched by the drawing.
    std::vector<std::vector<std::vector<Vec2>>> sections;
};

/// The plane a section view cuts the model with: what is on the `normal`
/// side of it (toward the viewer) is taken away.
struct CutPlane {
    Vec3 point, normal;
};

/// An edge of the model as it is: the ends of a line, the centre, axis and
/// radius of a circle or an arc (drawing dimensions, ADR-0012 step 5).
struct ModelEdge {
    enum class Kind : unsigned char { Line, Circle, Other } kind = Kind::Other;
    Vec3   a, b;
    Vec3   center, axis;
    double radius = 0;
};

/// An edge picked on a view: what it is now and how to find it again.
struct PickedEdge {
    doc::EdgeReference ref;
    ModelEdge          edge;
};

/// The bodies of a model copied, so the lines can be computed on another
/// thread while the model goes on changing. Cheap to copy (shared).
class ViewShapes {
public:
    ViewShapes() = default;
    static ViewShapes Of(const Model& model);
    bool Empty() const { return !myData; }

    /// `eye` — toward the viewer, `up` — up on the sheet (DrawingView.h,
    /// BasisOf); x of the result is up × eye. Smooth edges (between faces
    /// that meet tangentially: fillets) are left out, as ЕСКД draws them.
    /// `cut` — a section: the model cut first, its faces on the plane given too.
    Expected<ProjectedLines> Project(Vec3 eye, Vec3 up, const std::optional<CutPlane>& cut = std::nullopt) const;

    /// The edge of the bodies nearest to `p` (the view's plane at 1:1, as
    /// Project gives it) within `tolerance`; of edges that look the same from
    /// here, the one nearer the viewer. nullopt — none that near.
    std::optional<PickedEdge> EdgeAt(Vec3 eye, Vec3 up, Vec2 p, double tolerance) const;
    /// The edges of `refs` as they are now (kernel::occt::ResolveEdges); nullopt for one not found.
    std::vector<std::optional<ModelEdge>> Resolve(const std::vector<doc::EdgeReference>& refs) const;

    struct Data;

private:
    std::shared_ptr<const Data> myData;
};

} // namespace bz::kernel
