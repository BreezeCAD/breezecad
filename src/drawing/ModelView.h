#pragma once
// A view of a model on a sheet (ADR-0012, step 3): the main view and the
// views projected from it — from above and from the left, placed by
// ГОСТ 2.305 (first angle: the view from above is under the main one, the
// view from the left is to its right).
//
// In the file a view is an object of Sheet::views with "type": "modelView":
//
//   {"type": "modelView", "id": 7, "model": 2, "projection": "front",
//    "scale": [1, 2], "at": [120.0, 190.0], "hiddenLines": true}
//   {"type": "modelView", "id": 8, "model": 2, "projection": "top",
//    "parent": 7, "at": [120.0, 95.0], "hiddenLines": true}
//
// Only what the user chose is kept; the lines are computed from the model
// (drawing/ViewLines.h). The format rules of ADR-0012 hold: these keys keep
// their names and meaning; keys this version does not know stay in the
// object; a view with a projection it does not know is left alone as an
// unknown one. Format version 1 still: a program without views reads such a
// file right — it keeps the views and says they are from a newer version.

#include "core/Id.h"
#include "core/Math.h"
#include "drawing/Drawing.h"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bz::drawing {

inline constexpr const char* kModelViewType = "modelView";

/// Which side of the model is drawn. The main view is the program's «Вид
/// спереди»; the other two are what ГОСТ 2.305 calls «вид сверху» and «вид слева».
enum class ViewProjection : unsigned char { Front, Top, Left };

/// A scale of ГОСТ 2.302 as written: 1:2, 2,5:1.
struct Scale {
    double image = 1, object = 1;   ///< image : object

    double Factor() const { return object > 0 ? image / object : 1.0; }
    bool operator==(const Scale&) const = default;
};

/// "1:2", "2,5:1" — as in the title block (ГОСТ 2.302).
std::string ScaleText(Scale scale);
/// The scales of ГОСТ 2.302 from the largest (100:1) to the smallest (1:1000).
std::span<const Scale> StandardScales();

struct ModelView {
    ObjectId       id;
    ObjectId       model;    ///< ModelRef::id — the part drawn
    ViewProjection projection = ViewProjection::Front;
    /// Of its own for a main view; a projected one takes its parent's (views
    /// in projection have one scale) and keeps none in the file.
    std::optional<Scale> scale;
    /// Where the model's origin is on the sheet, mm. Of a projected view only
    /// the coordinate along its link counts: the other one is the parent's.
    Vec2     at;
    ObjectId parent;         ///< the main view it is projected from; not valid for a main view
    bool     hiddenLines = true;   ///< lines of the invisible contour are drawn
};

/// True for a view object of this kind that this version can show.
bool IsModelView(const Item& item);
/// The view an item holds; nullopt for another type or a projection of a newer version.
std::optional<ModelView> ReadModelView(const Item& item);
/// Puts the view into the item; keys of a newer version in it stay.
void StoreModelView(Item& item, const ModelView& view);
Item MakeViewItem(const ModelView& view);

/// The views of the sheet this version shows, in the order drawn.
std::vector<ModelView> ModelViewsOf(const Sheet& sheet);
/// The item of the view `id` on the sheet; nullptr when there is none.
Item*       FindViewItem(Sheet& sheet, ObjectId id);
const Item* FindViewItem(const Sheet& sheet, ObjectId id);
/// Views and annotations of the sheet this version cannot show (they are kept).
std::size_t UnknownItemCount(const Sheet& sheet);
/// The same over the whole drawing.
std::size_t UnknownItemCount(const Drawing& drawing);

/// «Главный вид», «Вид сверху», «Вид слева».
std::string_view ProjectionName(ViewProjection projection);

} // namespace bz::drawing
