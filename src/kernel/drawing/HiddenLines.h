#pragma once
// The lines of a drawing view (ADR-0012, step 3): the model's edges and
// outlines seen from a direction, visible and hidden apart (OCCT's exact
// hidden line removal, HLRBRep_Algo), and the centre lines of its round
// holes and shafts. All in the view's plane at 1:1, the model's origin at
// (0, 0); the drawing scales and places them (drawing/SheetLayout.h).

#include "core/Expected.h"
#include "core/Math.h"

#include <memory>
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
    Expected<ProjectedLines> Project(Vec3 eye, Vec3 up) const;

    struct Data;

private:
    std::shared_ptr<const Data> myData;
};

} // namespace bz::kernel
