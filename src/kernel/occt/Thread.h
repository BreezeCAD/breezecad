#pragma once

#include "core/Expected.h"
#include "document/ThreadFeature.h"

#include <TopoDS_Shape.hxx>

namespace bz::kernel::occt {

/// What a thread on a cylindrical face of `body` would be.
struct ThreadSite {
    bool   external = true;   ///< a shaft (the groove goes in), not a hole
    double nominal = 0;       ///< nominal diameter: the shaft, or the hole plus the thread depth
    double length = 0;        ///< length of the face along its axis
};

/// The cylindrical face `ref` refers to on `body`, described.
Expected<ThreadSite> FindThreadSite(const TopoDS_Shape& body, const doc::CylinderTouch& ref);

/// What a thread was like, besides its tool.
struct ThreadNotes {
    double web = 1e300;     ///< mm of material left between two turns of the groove at the surface
    bool   cached = false;  ///< the tool came from the cache (ToolCache.h)
};

/// The groove of the thread of `feature` on `body` (what is cut out).
Expected<TopoDS_Shape> ThreadTool(const TopoDS_Shape& body, const doc::ThreadFeature& feature, ThreadNotes* notes = nullptr);

/// A cosmetic thread: the helix drawn on the face, nothing cut.
Expected<TopoDS_Shape> CosmeticThread(const TopoDS_Shape& body, const doc::ThreadFeature& feature);

} // namespace bz::kernel::occt
