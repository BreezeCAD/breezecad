#pragma once

#include "core/Expected.h"
#include "document/ExtrudeFeature.h"

#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>

#include <optional>
#include <vector>

namespace bz::kernel::occt {

/// The solid swept by an extrusion of `faces` (before any boolean).
/// `throughAllLength` is used for ExtentType::ThroughAll; `target` is the
/// plane of ExtentType::ToPlane, or `untilFace` a curved face to go up to
/// (found by the caller).
Expected<TopoDS_Shape> MakeExtrusion(const std::vector<TopoDS_Face>& faces,
                                     const doc::ExtrudeFeature& feature,
                                     const Frame& sketchFrame,
                                     double throughAllLength,
                                     const std::optional<Frame>& target = std::nullopt,
                                     const std::optional<TopoDS_Face>& untilFace = std::nullopt);

/// Union of two shapes. Throws Standard_Failure like BRepAlgoAPI_Fuse when OCCT fails.
TopoDS_Shape Fuse(const TopoDS_Shape& a, const TopoDS_Shape& b);

/// What a boolean was like, besides its result.
struct BooleanNote {
    bool touched = true;    ///< a cut: the tool reached the target's material (false — the target came back as it was)
    bool partial = false;   ///< a cut: material is left where the tool is, however it was tried
    bool retried = false;   ///< made with a tolerance: the exact one failed or was not complete
};

/// Applies a boolean of `tool` to `target`. Returns the new body shape.
/// A result that is not valid — and a cut that did not remove what the tool
/// covers — is tried again with a tolerance for surfaces that only touch;
/// a cut still incomplete after that is returned with `note->partial`.
Expected<TopoDS_Shape> ApplyBoolean(const TopoDS_Shape& target, const TopoDS_Shape& tool, doc::BooleanOp op,
                                    BooleanNote* note = nullptr);
/// Joins or cuts several tools at once (they may overlap each other), as
/// patterns need. NewBody and Intersect are not accepted here.
Expected<TopoDS_Shape> ApplyBoolean(const TopoDS_Shape& target, const std::vector<TopoDS_Shape>& tools, doc::BooleanOp op);

/// Where a boolean of `tool` turned `before` into `after` with a new face
/// narrower than 0.05 mm (two surfaces that almost, but not quite,
/// coincide: a pocket 6.15 deep under a wall at 6.12): such a strip is seen
/// as a line on the model and stops fillets. None when there is none.
std::optional<Vec3> FindNewSliver(const TopoDS_Shape& before, const TopoDS_Shape& after, const TopoDS_Shape& tool);

/// Merges coplanar faces and collinear edges left by booleans, so the model
/// keeps a clean topology (one face per flat side).
TopoDS_Shape Clean(const TopoDS_Shape& shape);

} // namespace bz::kernel::occt
