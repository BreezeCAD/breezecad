#pragma once
// The sheet as lines and texts: its size (ГОСТ 2.301), the frame and the
// title block of form 1 with the additional graphs (ГОСТ 2.104-2006). The
// screen, PDF and DXF draw the same list, so the three never disagree.
//
// Millimetres on the sheet, origin at its lower left corner, y up. Graph
// positions follow KiCad's ГОСТ sheet (template/gost_landscape.kicad_wks,
// «in accordance of the standard GOST 2.104-2006»), measured there from the
// lower right corner of the frame.
//
// Views (DrawingView.h) come as polylines in the same millimetres.

#include "core/Math.h"
#include "drawing/Drawing.h"
#include "drawing/DrawingView.h"

#include <functional>
#include <string>
#include <vector>

namespace bz::drawing {

/// ГОСТ 2.303: the solid main line and the solid thin one.
enum class LineWeight : unsigned char { Thick, Thin };

struct Stroke {
    Vec2       a, b;
    LineWeight weight = LineWeight::Thick;
};

/// ГОСТ 2.303: solid, dashed (hidden contour), dash-dot (centre lines).
enum class LineStyle : unsigned char { Solid, Dashed, DashDot };

/// A line of a view: points joined in order. A dashed style runs along the
/// whole of it, as a plotter draws it.
struct Polyline {
    std::vector<Vec2> points;
    LineWeight        weight = LineWeight::Thick;
    LineStyle         style = LineStyle::Solid;
}; 

enum class Align : unsigned char { Left, Center };

/// A text of the sheet: a fixed caption («Разраб.») or a field value.
struct Label {
    std::string text;
    std::string field;          ///< the title block field it shows (field::…); empty for a caption
    Vec2        at;             ///< left end or centre of the base line… see `align`
    double      height = 2.5;   ///< ГОСТ 2.304 font size, mm
    Align       align = Align::Left;
    double      rotation = 0;   ///< degrees, counter-clockwise
    double      maxWidth = 0;   ///< the cell's width: longer text is squeezed to it; 0 — no limit
};

struct Box {
    Vec2 min, max;
};

struct SheetLayout {
    double width = 0, height = 0;   ///< the sheet, mm
    Box    frame;                   ///< inside the frame: where views go
    Box    titleBlock;              ///< 185 × 55, the lower right corner of the frame
    std::vector<Stroke> strokes;
    std::vector<Label>  labels;
    std::vector<Polyline> polylines;   ///< the views
};

/// What the kernel computed of a view (kernel/drawing): at 1:1, in the
/// view's plane (x right, y up), the model's origin at (0, 0).
struct ViewLines {
    std::vector<std::vector<Vec2>> visible, hidden;
    /// A round hole or shaft seen end on: a cross of centre lines over it.
    struct Mark {
        Vec2   center;
        double radius = 0;
    };
    /// One seen from the side: its axis, end to end.
    struct Axis {
        Vec2 a, b;
    };
    std::vector<Mark> marks;
    std::vector<Axis> axes;
};

/// The view's lines on the sheet: scaled, moved to `view.at`, styled.
void AddView(SheetLayout& out, const View& view, const ViewLines& lines);
/// The box of the view on the sheet, centre lines included; empty without lines.
Box2 ViewBox(const View& view, const ViewLines& lines);

/// The lines of a view, or nullptr while they are not known (not computed
/// yet, the model is missing): such a view is left out.
using ViewLinesSource = std::function<const ViewLines*(const Sheet&, const View&)>;
/// LayoutSheet and the views of the sheet over it.
SheetLayout LayoutSheetWithViews(const Sheet& sheet, int number, int count, const ViewLinesSource& lines);

/// Width and height of the sheet in mm. A4 is upright always: its title
/// block runs along the short side (ГОСТ 2.104).
Vec2 SheetSizeMm(SheetSize size, Orientation orientation);

/// The sheet `number` of `count` with its frame, title block (form 1) and
/// additional graphs; field values from the sheet's title block, sheet
/// numbers from the arguments.
SheetLayout LayoutSheet(const Sheet& sheet, int number, int count);

} // namespace bz::drawing
