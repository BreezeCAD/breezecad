#pragma once
// Views of a model in an open drawing (ADR-0012, step 3) — the part without
// the screen: the models of the drawing built for their views, the lines of
// each view kept until its model changes, and the changes to views (the
// standard three, move, scale, hidden lines, delete), each one undoable step.
// The mouse and the picture are in DrawingViewsUi.
//
// A view follows its model. A part open in a tab is taken from there, as it
// is now (its revision is watched); a part on disk is read and built here,
// and read again when the file changes.

#include "core/Expected.h"
#include "core/Id.h"
#include "drawing/ModelView.h"
#include "drawing/ViewLayout.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace bz::doc {
class Document;
}
namespace bz::kernel {
class Model;
}

namespace bz::app {

class DrawingState;
class Session;

/// The documents open in tabs (Workspace::sessions): a part among them is drawn as it is now.
using OpenSessions = std::vector<std::unique_ptr<Session>>;

class DrawingViewCache {
public:
    DrawingViewCache();
    ~DrawingViewCache();

    /// Brings the lines up to date for every view of the drawing: reads and
    /// builds models not seen yet or changed, projects what is missing.
    /// `open` — the tabs, to take parts open there; without it files only.
    void Refresh(const DrawingState& d, const OpenSessions* open);
    /// The lines by view id, as of the last Refresh.
    const drawing::ViewImages& Images() const { return myImages; }
    /// Grows whenever Images() changes.
    std::uint64_t Generation() const { return myGeneration; }
    /// Looks at the files again on the next Refresh, not waiting for the usual pause.
    void CheckFilesNow();
    /// Why the views of the model show nothing; empty when they do.
    std::string Problem(ObjectId model) const;

    /// The lines of the model seen along `projection` — for a view not on the sheet yet.
    Expected<std::shared_ptr<const drawing::ViewLines>> Lines(const DrawingState& d, const OpenSessions* open, ObjectId model,
                                                               drawing::ViewProjection projection);

private:
    struct Source;
    Source* Update(const DrawingState& d, const OpenSessions* open, ObjectId model);
    std::shared_ptr<const drawing::ViewLines> Project(Source& source, drawing::ViewProjection projection);

    std::map<ObjectId, std::unique_ptr<Source>> mySources;   // by ModelRef id
    drawing::ViewImages myImages;
    std::uint64_t myGeneration = 0;
};

/// What the editor keeps about the views of an open drawing besides the
/// document itself (DrawingState::views).
struct DrawingViewsState {
    DrawingViewCache cache;
    ObjectId selected;      ///< the view picked on the sheet or in the tree
    // A view being moved with the mouse: shown at `dragAt`, written on release.
    ObjectId dragged;
    bool     dragMoved = false;
    Vec2     dragFrom;      ///< the cursor on the sheet when the button went down, mm
    Vec2     dragOrigin;    ///< the view's place then
    Vec2     dragAt;        ///< the view's place now
    ObjectId hovered;       ///< the view under the cursor

    // The views of the sheet shown as lines, kept until something they depend on changes.
    std::vector<drawing::PlacedView> placed;
    std::uint64_t placedRevision = ~std::uint64_t{0}, placedGeneration = 0;
    ObjectId      placedSheet;
    bool          placedDragging = false;
    Vec2          placedDragAt;
};

/// The views of the sheet shown, placed (with the one being dragged where the cursor has it).
const std::vector<drawing::PlacedView>& PlacedViews(DrawingState& d);

/// The state of the drawing's views, made on first use.
DrawingViewsState& ViewsOf(DrawingState& d);

/// The file of a model of the drawing: its kept path against the drawing's own file.
std::filesystem::path ModelFile(const DrawingState& d, const drawing::ModelRef& model);

/// «Стандартные виды»: the main view, the view from above and the view from
/// the left of the drawing's part on the sheet shown, at a scale of
/// ГОСТ 2.302 that fits them. Returns the main view's id; an error says why not.
Expected<ObjectId> AddStandardViews(DrawingState& d, const OpenSessions* open);
/// Puts the view at `at`: a projected one keeps to its link (only its distance from the
/// main view changes), a main one takes its projected views along.
void MoveView(DrawingState& d, ObjectId view, Vec2 at);
/// The scale of the view and of those in projection with it; the title block follows
/// if it showed the old scale.
void SetViewScale(DrawingState& d, ObjectId view, drawing::Scale scale);
void SetViewHiddenLines(DrawingState& d, ObjectId view, bool shown);
/// Removes the view and the views projected from it.
void RemoveView(DrawingState& d, ObjectId view);

/// The lines of all views for an export: brought up to date first.
const drawing::ViewImages& DrawingViewImages(DrawingState& d, const OpenSessions* open);

} // namespace bz::app
