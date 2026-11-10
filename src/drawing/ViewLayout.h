#pragma once
// Views on the sheet as lines (ADR-0012, step 3): each view's lines scaled
// and put in place, seen ones as the solid main line and hidden ones as the
// dashed line of ГОСТ 2.303. The screen and PDF draw this list, as they do
// the frame (SheetLayout.h), so the two never disagree.
//
// Also where views go: the projection link of ГОСТ 2.305 (a view from above
// stays under its main view, a view from the left beside it) and the first
// arrangement of the three standard views with a scale of ГОСТ 2.302.

#include "drawing/ModelView.h"
#include "drawing/SheetLayout.h"
#include "drawing/ViewLines.h"

#include <map>
#include <memory>
#include <optional>
#include <vector>

namespace bz::drawing {

/// The computed lines of views, by view id. A view without an entry has not
/// been computed (the model is not found or not built): it is an empty box.
using ViewImages = std::map<ObjectId, std::shared_ptr<const ViewLines>>;

/// The dashed line of ГОСТ 2.303 (линия невидимого контура): dashes 2…8 mm,
/// gaps 1…2 mm, thickness s/3…s/2 (drawn with LineWeight::Thin). The values
/// inside these ranges are this program's choice.
inline constexpr double kDashLength = 4.0;   ///< mm on the sheet
inline constexpr double kDashGap    = 1.5;   ///< mm on the sheet

/// A line of a view on the sheet: a run of points drawn with one pen. A
/// hidden line is already cut into its dashes.
struct ViewPath {
    Polyline   points;   ///< sheet mm
    LineWeight weight = LineWeight::Thick;
};

struct PlacedView {
    ModelView view;            ///< `at` and `scale` as they are on the sheet (through the parent)
    Scale     scale;
    Box       box;             ///< around its lines on the sheet; a small one for a view not computed
    bool      computed = false;
    std::vector<ViewPath> paths;
};

/// Every view of the sheet this version shows, placed, in the order drawn.
std::vector<PlacedView> LayoutViews(const Sheet& sheet, const ViewImages& images);

/// Where a view is on the sheet and at what scale, the projection link
/// applied: a view from above takes its parent's x, a view from the left
/// its parent's y, both the parent's scale. A view whose parent is gone
/// stands by itself.
struct ViewPlace {
    Vec2  at;
    Scale scale;
};
ViewPlace PlaceOf(const Sheet& sheet, const ModelView& view);
/// `at` as a view in projection may keep it: a view from above moves only up
/// and down, a view from the left only sideways; any other view goes anywhere.
Vec2 KeepLink(const Sheet& sheet, const ModelView& view, Vec2 at);
/// Moves the view `id` of the sheet to `at`, keeping its link. The views
/// projected from it go with it: their distances from it stay.
void MoveViewTo(Sheet& sheet, ObjectId id, Vec2 at);

/// `line` cut into dashes of about `dash` with gaps of `gap`: the count is
/// fitted so the line begins and ends with a whole dash. A line too short
/// for two dashes is one stroke.
std::vector<Polyline> Dashed(const Polyline& line, double dash = kDashLength, double gap = kDashGap);

/// The first arrangement of the standard views: the main one, the view from
/// above under it and the view from the left to its right (ГОСТ 2.305,
/// first angle), at a scale of ГОСТ 2.302 that fits them into the frame clear
/// of the title block: 1:1 when they fit, the nearest reduction when they do
/// not, an enlargement only for a part that would be lost on the sheet at 1:1.
struct StandardViewsPlan {
    Scale scale;
    Vec2  front, top, left;   ///< `at` of each view
};
std::optional<StandardViewsPlan> PlanStandardViews(const SheetLayout& sheet, const ViewLines& front, const ViewLines& top,
                                                   const ViewLines& left);

} // namespace bz::drawing
