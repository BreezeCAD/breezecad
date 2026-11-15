#pragma once
// A dimension of a drawing view (ADR-0012, step 5): linear, diameter, radius
// or angle, tied to edges of the model, not to lines of the view. Its value,
// extension lines and arrows are never stored — they are computed from the
// model every time (as the view's lines), so a changed part changes the size.
//
// In the file a dimension is an item of the sheet's "annotations":
//   {"type": "dimension", "id": 9, "view": 7, "kind": "linear", "axis": "horizontal",
//    "edges": [{…EdgeReference…}, {…}], "at": [40, 25], "text": "<>"}
// "edges" — one or two references to model edges (document/JsonIO, as the
// part keeps its picks); "at" — where the dimension line passes (its text
// stands there), in the view's plane at 1:1 from the model's origin, so the
// dimension moves and scales with its view; "text" — the user's text, "<>"
// standing for the value; none — the value alone. Keys this version does not
// know stay in the item and are written back.
//
// An edge that is not found any more does not delete the dimension: it is
// drawn red where its edges were and waits to be tied again (ADR-0012).

#include "core/Id.h"
#include "core/Math.h"
#include "drawing/Drawing.h"
#include "drawing/DrawingView.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bz::drawing {

struct SheetLayout;

inline constexpr const char* kDimensionType = "dimension";

enum class DimensionKind : unsigned char { Linear, Diameter, Radius, Angle };
/// A linear dimension measures along the sheet's x, its y, or along the line
/// between its points (КОМПАС: горизонтальный, вертикальный, параллельный).
enum class DimensionAxis : unsigned char { Horizontal, Vertical, Aligned };

std::string_view KindId(DimensionKind kind);
std::optional<DimensionKind> KindFromId(std::string_view id);
std::string_view AxisId(DimensionAxis axis);
std::optional<DimensionAxis> AxisFromId(std::string_view id);

struct Dimension {
    ObjectId          id;
    ObjectId          view;
    DimensionKind     kind = DimensionKind::Linear;
    DimensionAxis     axis = DimensionAxis::Aligned;
    std::vector<Json> edges;   ///< EdgeReference as JSON (document/JsonIO): read by the app, kept here as is
    Vec2              at;      ///< the dimension line passes here: view plane, 1:1, model origin at (0, 0)
    std::string       text;    ///< the user's text, "<>" — the value; empty — the value alone
};

std::optional<Dimension> ReadDimension(const Item& item);
/// Writes the dimension's keys into the item; its other keys stay.
void WriteDimension(Item& item, const Dimension& dimension);
Item MakeDimensionItem(const Dimension& dimension);

/// The dimensions of the sheet that this version reads, in drawing order.
std::vector<Dimension> DimensionsOf(const Sheet& sheet);
Item* FindAnnotation(Sheet& sheet, ObjectId id);

/// An edge of the model as it is now (found by the kernel from its reference).
struct ModelEdge {
    enum class Kind : unsigned char { Line, Circle, Other } kind = Kind::Other;
    Vec3   a, b;           ///< its ends (a full circle: the same point)
    Vec3   center, axis;   ///< a circle or an arc
    double radius = 0;
};

/// What a dimension measures in its view, on the sheet (mm).
struct Measure {
    DimensionKind kind = DimensionKind::Linear;
    double value = 0;        ///< mm (true size in the view's plane), or degrees for an angle
    Vec2   p1, p2;           ///< linear: the points measured; angle: on its two lines
    Vec2   center;           ///< diameter, radius: the circle's centre; angle: where the lines meet
    double radius = 0;       ///< diameter, radius: of the circle on the sheet
    bool   endOn = false;    ///< diameter, radius: the circle is seen as a circle (else as a line, from its side)
    Vec2   u1, u2;           ///< angle: directions of its sides from `center`
    std::string prefix;      ///< «Ø», «R»
};

/// Measures `dim` in `view`; nullopt when its edges are not there (`edges`
/// holds nullopt for those) or do not make such a dimension (an angle of
/// parallel lines, a radius of a straight edge). `why` says which.
std::optional<Measure> MeasureOf(const View& view, const Dimension& dim, const std::vector<std::optional<ModelEdge>>& edges,
                                 std::string* why = nullptr);

/// The value as written on a drawing (ГОСТ 2.307): «12», «12,5», «0,25»;
/// an angle in degrees and minutes: «30°», «12°30'».
std::string ValueText(const Measure& m);
/// The text shown: the user's with «<>» replaced, or the prefix and value.
std::string DimensionText(const Dimension& dim, const Measure& m);

/// The dimension on the sheet: extension and dimension lines, arrows, text.
/// `measure` nullopt — its edges are lost: drawn red, as a leader from its
/// text to where its edges were (their middle points, `lost`), on the sheet.
/// Returns the box it takes (the editor picks it there).
Box2 AddDimension(SheetLayout& out, const View& view, const Dimension& dim, const std::optional<Measure>& measure,
                  const std::vector<Vec2>& lost = {});

/// Where the dimension's text stands on the sheet.
Vec2 DimensionAnchor(const View& view, const Dimension& dim);
/// The point of the view's plane (1:1) under a sheet point.
Vec2 ViewPointOf(const View& view, Vec2 sheet);
/// A model point on the sheet.
Vec2 SheetPointOf(const View& view, Vec3 model);

/// ГОСТ 2.307 sizes chosen for the program (mm): not checked against the
/// text of the standard, see the PR of step 5.
inline constexpr double kDimTextHeight = 3.5;
inline constexpr double kArrowLength   = 3.0;
inline constexpr double kArrowHalfWidth = 0.5;
inline constexpr double kExtensionOver = 2.0;   ///< an extension line goes past the dimension line

} // namespace bz::drawing
