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
#include "drawing/Dimension.h"
#include "drawing/DrawingView.h"
#include "drawing/Specification.h"

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace bz::drawing {

/// Title block fields by key (field::…).
using Fields = std::map<std::string, std::string>;

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
    bool              problem = false;   ///< a dimension whose edges are lost: red on the screen
};

/// A filled shape: an arrow of a dimension (ГОСТ 2.307). Convex.
struct Fill {
    std::vector<Vec2> points;
    bool              problem = false;
};

/// What the editor can pick on the sheet: an annotation and the box it takes.
struct Hit {
    ObjectId id;
    Box2     box;
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
    bool        problem = false;   ///< red on the screen (a dimension whose edges are lost)
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
    std::vector<Polyline> polylines;   ///< the views and their dimensions
    std::vector<Fill>     fills;       ///< arrows
    std::vector<Hit>      hits;        ///< the annotations, for picking
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
    /// A section view: the faces on the cutting plane, each as its loops — hatched.
    std::vector<std::vector<std::vector<Vec2>>> sections;
};

/// ГОСТ 2.306 hatching of a cut face (metal: thin lines at 45°): `loops`
/// on the sheet, even-odd (holes stay clear); `step` — between lines, mm.
std::vector<Polyline> Hatch(const std::vector<std::vector<Vec2>>& loops, double step, double angleDeg = 45);
/// The cutting line of `section` on its parent view (ГОСТ 2.305, 2.303):
/// thick strokes at its ends, arrows of the sight, the letters.
void AddCuttingLine(SheetLayout& out, const View& parent, const View& section);
/// The «А–А» over a section view whose box on the sheet is `box`.
void AddSectionLabel(SheetLayout& out, const View& section, const Box2& box);

/// The view's lines on the sheet: scaled, moved to `view.at`, styled.
void AddView(SheetLayout& out, const View& view, const ViewLines& lines);
/// The box of the view on the sheet, centre lines included; empty without lines.
Box2 ViewBox(const View& view, const ViewLines& lines);

/// The lines of a view, or nullptr while they are not known (not computed
/// yet, the model is missing): such a view is left out.
using ViewLinesSource = std::function<const ViewLines*(const Sheet&, const View&)>;

/// What a sheet shows that comes from its models: the views' lines, the
/// title block fields of the model, the specification's rows. A part not
/// given is left out (frames only).
struct SheetContent {
    ViewLinesSource                                   lines;
    /// The model edges of a dimension of the view now (Dimension.h); none
    /// given — dimensions are left out.
    std::function<std::vector<std::optional<ModelEdge>>(const Sheet&, const View&, const std::vector<Json>&)> edges;
    std::function<Fields(const Sheet&)>               fields;
    std::function<std::vector<SpecRow>(const Sheet&)> spec;
};

/// A page of the drawing as printed: a drawing sheet, or one page of a
/// specification. Drawing sheets are numbered among themselves, the pages
/// of a specification among themselves (it is a document of its own).
struct Page {
    std::size_t sheet = 0;   ///< index in Drawing::sheets
    int         page = 0;    ///< of a specification, from 0
    int         number = 1, count = 1;
};
std::vector<Page> PagesOf(const Drawing& drawing, const SheetContent& content);
/// The pages of one sheet (its specification may take several).
std::vector<Page> PagesOf(const Drawing& drawing, std::size_t sheet, const SheetContent& content);
/// The page laid out: frame, title block, views or the table.
SheetLayout LayoutPage(const Drawing& drawing, const Page& page, const SheetContent& content);

/// Width and height of the sheet in mm. A4 is upright always: its title
/// block runs along the short side (ГОСТ 2.104).
Vec2 SheetSizeMm(SheetSize size, Orientation orientation);

/// The sheet `number` of `count` with its frame, title block (form 1) and
/// additional graphs; field values from the sheet's title block, a field it
/// does not set from `automatic` (the model's: Drawing.h, TitleBlock), sheet
/// numbers from the arguments.
SheetLayout LayoutSheet(const Sheet& sheet, int number, int count, const Fields* automatic = nullptr);

/// Page `page` (from 0) of `pages` of a specification (ГОСТ 2.106 form 1 on
/// A4): the table with its part of `rows`, the title block of form 2 on the
/// first page and of form 2a on the next ones.
SheetLayout LayoutSpecPage(const Sheet& sheet, const std::vector<SpecRow>& rows, int page, int pages, const Fields* automatic = nullptr);

} // namespace bz::drawing
