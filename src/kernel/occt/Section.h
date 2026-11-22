#pragma once

#include "core/Expected.h"
#include "core/Math.h"

#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>

#include <optional>
#include <vector>

namespace bz::kernel::occt {

/// What is left of `body` on one side of `plane`: behind it (against the
/// normal), or in front when `keepFront`. Nullopt when nothing is left.
Expected<std::optional<TopoDS_Shape>> CutByPlane(const TopoDS_Shape& body, const Frame& plane, bool keepFront);

/// Where `plane` cuts the material of `body`: the flat faces of the cut,
/// facing along the plane's normal and meshed for display (`deflection`, mm).
/// Empty when the plane misses the body. For the hatched cut of "Разрез вида".
Expected<std::vector<TopoDS_Face>> CutFaces(const TopoDS_Shape& body, const Frame& plane, double deflection);

} // namespace bz::kernel::occt
