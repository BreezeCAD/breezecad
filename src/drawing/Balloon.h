#pragma once
// A position leader of an assembly drawing (ГОСТ 2.109): the number of a
// component's row of the specification on a shelf, a thin leader line from
// it to a dot on the component. The number is never stored — it comes from
// the specification (Specification.h), so renumbered rows renumber the
// drawing; the dot follows the component when it moves in the assembly.
//
// In the file a position is an item of the sheet's "annotations":
//   {"type": "balloon", "id": 12, "view": 7, "component": 31,
//    "point": [10, 0, 5], "at": [80, 60]}
// "component" — its id in the assembly; "point" — the dot, in the
// component's part coordinates; "at" — the start of the shelf, in the view's
// plane at 1:1 from the model's origin (it moves and scales with its view).
// Keys this version does not know stay in the item and are written back.

#include "core/Id.h"
#include "core/Math.h"
#include "drawing/Drawing.h"
#include "drawing/DrawingView.h"

#include <optional>
#include <string>
#include <vector>

namespace bz::drawing {

struct SheetLayout;

inline constexpr const char* kBalloonType = "balloon";

struct Balloon {
    ObjectId id;
    ObjectId view;
    ObjectId component;
    Vec3     point;   ///< the dot, in the component's part coordinates
    Vec2     at;      ///< the shelf's start: view plane, 1:1, model origin at (0, 0)
};

std::optional<Balloon> ReadBalloon(const Item& item);
void WriteBalloon(Item& item, const Balloon& balloon);
Item MakeBalloonItem(const Balloon& balloon);
std::vector<Balloon> BalloonsOf(const Sheet& sheet);

/// What the assembly says of a position now: its number in the
/// specification and where its dot is in the assembly.
struct BalloonTarget {
    std::string number;
    Vec3        anchor;
};

/// ГОСТ 2.109 sizes chosen for the program (mm), not checked against the text:
/// the number a size or two over the dimension figures, a dot of 1 mm.
inline constexpr double kBalloonTextHeight = 7.0;
inline constexpr double kBalloonDot = 0.5;   ///< radius

/// The position on the sheet: dot, leader, shelf and number. nullopt — the
/// component is gone from the assembly: a red «?» on the shelf. Returns its box.
Box2 AddBalloon(SheetLayout& out, const View& view, const Balloon& balloon, const std::optional<BalloonTarget>& target);

/// Shelves for new positions of a view: beside its box `box` (sheet mm) on
/// the side nearer the sheet's free room (right, else left), at least
/// `gap` apart, in the order of their dots' heights. Returns sheet points.
std::vector<Vec2> ArrangeShelves(const Box2& box, const std::vector<Vec2>& dots, double sheetWidth, double gap = 10);

} // namespace bz::drawing
