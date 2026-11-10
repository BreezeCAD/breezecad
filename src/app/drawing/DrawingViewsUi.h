#pragma once
// Views of a model on the sheet of the drawing editor (ADR-0012, step 3):
// the picture, the mouse (pick, drag along the projection link, the menu of
// a view) and the «Стандартные виды» command. The state and the changes
// themselves are in DrawingViews; the editor (DrawingEditor) calls in here.

#include "app/AppContext.h"
#include "core/Id.h"
#include "core/Math.h"

#include <imgui.h>

#include <string>
#include <vector>

namespace bz::app {

class DrawingState;
class Session;

/// How the sheet is shown: the same numbers the editor draws the frame with.
struct SheetCamera {
    ImVec2 middle;   ///< the canvas centre, px
    Vec2   center;   ///< sheet point there, mm
    double zoom;     ///< px per mm

    ImVec2 ToScreen(Vec2 p) const {
        return {static_cast<float>(middle.x + (p.x - center.x) * zoom), static_cast<float>(middle.y - (p.y - center.y) * zoom)};
    }
    Vec2 ToSheet(ImVec2 p) const { return {center.x + (p.x - middle.x) / zoom, center.y - (p.y - middle.y) / zoom}; }
};

/// drawing.standardViews, drawing.removeView.
void RegisterDrawingViewActions(AppContext& app);

/// Call right after the sheet's canvas item: brings the views up to date with
/// their models and takes the mouse — a click picks a view, a drag moves it,
/// the right button opens its menu, Del removes it. True while a view is
/// held: the sheet must not move under it.
bool SheetViewsInput(AppContext& app, Session& session, const SheetCamera& camera, bool hovered);
/// The views over the sheet's paper.
void DrawSheetViews(AppContext& app, Session& session, ImDrawList* dl, const SheetCamera& camera);
/// The note over the sheet: what to do next, or what is wrong with a view.
std::string SheetViewsNote(DrawingState& d);

/// The views of the sheet shown, for the drawing's tree.
struct ViewRow {
    ObjectId    id;
    std::string title;   ///< «Главный вид (1:2)»
    bool        selected = false;
};
std::vector<ViewRow> SheetViewRows(DrawingState& d);
void SelectView(DrawingState& d, ObjectId view);

} // namespace bz::app
