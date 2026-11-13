#pragma once

#include "core/Expected.h"

#include <TopoDS_Wire.hxx>
#include <gp_Ax3.hxx>

namespace bz::kernel::occt {

/// How a helix is cut into edges. A sweep along it (MakePipeShell) makes as
/// many faces per side of the profile as there are edges, and every later
/// boolean on the body pays for each face.
enum class HelixSplit {
    HalfTurns,   ///< an edge per half turn (as before STAB-3)
    Turns,       ///< an edge per whole turn
    Single,      ///< one edge for the whole helix
};

/// The split coils and threads use: chosen by measurement (CoilTests,
/// "measure: helix split"; docs/dev/OCCT-ISSUES.md).
HelixSplit DefaultHelixSplit();
/// For the measurements only: what DefaultHelixSplit gives from now on.
void SetHelixSplitForMeasurement(HelixSplit split);

/// A helix on the cylinder of `radius` around `frame`'s axis (Z), starting
/// at its X direction: `turns` turns over `height` along the axis.
Expected<TopoDS_Wire> MakeHelix(const gp_Ax3& frame, double radius, double turns, double height, bool leftHanded,
                                HelixSplit split = DefaultHelixSplit());

} // namespace bz::kernel::occt
