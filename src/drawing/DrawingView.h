#pragma once
// A view of a model on a sheet (ADR-0012, step 3): which model, from where,
// at what scale and where on the sheet. Its lines are never stored — they
// are computed from the model (kernel/drawing), so the view follows the part.
//
// In the file a view is an item of the sheet's "views" with "type": "view":
//   {"type": "view", "id": 7, "model": 2, "direction": "front",
//    "scale": [1, 2], "at": [120.5, 180], "parent": 5, "hidden": true, "axes": true}
// "at" is where the model's origin lands on the sheet, mm; "parent" — the
// view this one is in projection with (moves along with it). Keys this
// version does not know stay in the item and are written back.

#include "core/Math.h"
#include "drawing/Drawing.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bz::drawing {

inline constexpr const char* kViewType = "view";

/// Where the view looks from (ЕСКД, ГОСТ 2.305; the first-angle method:
/// the top view goes under the front one, the left view to its right).
enum class ViewDirection : unsigned char { Front, Back, Top, Bottom, Left, Right, Iso };

std::string_view DirectionId(ViewDirection direction);
std::optional<ViewDirection> DirectionFromId(std::string_view id);

/// Toward the eye and up on the sheet, in model coordinates (Z up, the
/// front view looks along +Y, as «Вид спереди» of the part).
struct ViewBasis {
    Vec3 eye, up;
};
ViewBasis BasisOf(ViewDirection direction);
/// Model point → the view's plane (x right, y up), at 1:1, origin at the model's origin.
Vec2 ProjectPoint(ViewDirection direction, Vec3 p);

/// A scale «a:b» (ГОСТ 2.302): 1:2 is a = 1, b = 2.
struct Scale {
    double a = 1, b = 1;
    double Value() const { return a / b; }
    bool operator==(const Scale&) const = default;
};
/// «1:2», «2,5:1» — as the title block writes it.
std::string ScaleText(Scale scale);
/// The scales of ГОСТ 2.302, largest first.
const std::vector<Scale>& StandardScales();
/// The largest standard scale not above `factor`; the smallest one if none is.
Scale FitScale(double factor);

struct View {
    ObjectId      id;
    ObjectId      model;    ///< a ModelRef of the drawing
    ViewDirection direction = ViewDirection::Front;
    Scale         scale;
    Vec2          at;       ///< the model's origin on the sheet, mm
    ObjectId      parent;   ///< in projection with it; none for a main view
    bool          hidden = true;   ///< hidden lines (ГОСТ 2.303, dashed)
    bool          axes = true;     ///< centre lines of holes and shafts
};

/// The view an item holds, or nullopt when it is not one this version knows
/// (another type, or a view whose keys it cannot read): such an item is kept as is.
std::optional<View> ReadView(const Item& item);
/// Writes the view's keys into the item; its other keys stay.
void WriteView(Item& item, const View& view);
Item MakeViewItem(const View& view);

/// Whether this version shows the item (a view or a dimension it reads); others are kept as they are.
bool IsKnownItem(const Item& item);

/// The views of the sheet that this version reads, in drawing order.
std::vector<View> ViewsOf(const Sheet& sheet);
/// The item of the view `id` on the sheet; nullptr if none.
Item* FindViewItem(Sheet& sheet, ObjectId id);

} // namespace bz::drawing
