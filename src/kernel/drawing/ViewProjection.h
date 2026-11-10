#pragma once
// A view of a part for a drawing (ADR-0012, step 3): the bodies projected
// along one of the standard directions with hidden lines found. The lines
// come back as plain 2D polylines (drawing/ViewLines.h) — the drawing
// module itself knows nothing of OpenCASCADE.

#include "core/Expected.h"
#include "core/Math.h"
#include "drawing/ModelView.h"
#include "drawing/ViewLines.h"

namespace bz::kernel {

class Model;

/// The paper's axes in the model: where «right» and «up» of the sheet point.
/// The viewer looks against right × up.
struct ViewAxes {
    Vec3 right{1, 0, 0};
    Vec3 up{0, 0, 1};
};

/// The axes of a standard view. The main view is the program's «Вид
/// спереди» (looking along +Y, Z up). The other two follow ГОСТ 2.305, first
/// angle: the view from above looks down with the model's back toward the
/// main view; the view from the left looks along +X with the model's back
/// toward the main view too — so the three line up when laid out on the sheet.
ViewAxes AxesOf(drawing::ViewProjection projection);

/// All bodies of the model as seen along `axes`: sharp edges and silhouettes
/// of curved faces (a cylinder's sides, a fillet's outline), split into seen
/// and hidden. Smooth (tangent) edges — where a fillet meets a face — and
/// seams of closed faces are not drawn. An empty result for a model without
/// bodies; an error when OpenCASCADE fails.
Expected<drawing::ViewLines> ProjectView(const Model& model, const ViewAxes& axes);

} // namespace bz::kernel
