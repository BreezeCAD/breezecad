#pragma once

#include "core/Expected.h"
#include "core/Math.h"
#include "document/CoilFeature.h"
#include "sketch/Sketch.h"

#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>

#include <vector>

namespace bz::kernel::occt {

/// What a coil was like, besides its tool.
struct CoilNotes {
    double web = 1e300;     ///< mm left between two turns along the axis (a gap, or the web between two turns of a groove)
    bool   cached = false;  ///< the tool came from the cache (ToolCache.h), not swept again
};

/// The profile `faces` (sketch regions in model space) wound along a helix
/// around the feature's axis line of `sk` (sketch placed by `frame`).
Expected<TopoDS_Shape> MakeCoil(const std::vector<TopoDS_Face>& faces, const doc::CoilFeature& feature,
                                const sketch::Sketch& sk, const Frame& frame, CoilNotes* notes = nullptr);

} // namespace bz::kernel::occt
