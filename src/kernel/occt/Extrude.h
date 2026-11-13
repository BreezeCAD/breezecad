#pragma once

#include "core/Expected.h"
#include "document/ExtrudeFeature.h"
#include "kernel/occt/Ops.h"

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

// Booleans of the result with the bodies: kernel/occt/Ops.h (ApplyBoolean, Clean, FindNewSliver).

} // namespace bz::kernel::occt
