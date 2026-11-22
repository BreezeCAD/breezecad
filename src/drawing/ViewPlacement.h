#pragma once
// Where new views go on a sheet and at what scale (ADR-0012, step 3): the
// standard views of ЕСКД in projection with each other, and a free place for
// one more view. Pure geometry on the model's box; the editor calls it.

#include "core/Math.h"
#include "drawing/DrawingView.h"
#include "drawing/SheetLayout.h"

#include <optional>
#include <vector>

namespace bz::drawing {

/// The model's box seen from `direction` at 1:1, the model's origin at (0, 0).
Box2 Extent(ViewDirection direction, const Box3& model);
/// The same for a view (a section has its own direction).
Box2 Extent(const View& view, const Box3& model);

/// Where views may go: inside the frame (10 mm in), off the title block,
/// graph 26 and `taken` (views already there).
struct Room {
    Box2 area;
    std::vector<Box2> taken;
    bool Fits(const Box2& box) const;
};
Room RoomOf(const SheetLayout& sheet, const std::vector<Box2>& taken = {});

struct StandardPlacement {
    Scale scale;
    Vec2  front, top, left;   ///< the views' origins on the sheet (View::at)
};

/// ГОСТ 2.305, first angle: the front view, the top one under it, the left
/// one to its right, 20 mm apart (room for dimensions), at the largest
/// standard scale they fit at; the group centred, else at the top left.
std::optional<StandardPlacement> PlaceStandardViews(const Room& room, const Box3& model);

/// A free place for a box of `size`, the one nearest the top right corner on
/// a 5 mm grid: its lower left corner; nullopt if none.
std::optional<Vec2> FreeSpot(const Room& room, Vec2 size);

} // namespace bz::drawing
