#pragma once
// An open drawing (ADR-0012) in a Session: the document, the sheet shown,
// undo and the camera of the sheet. The editor is app/drawing/DrawingEditor.

#include "core/Id.h"
#include "core/Math.h"
#include "drawing/Drawing.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace bz::app {

class DrawingViews;

class DrawingState {
public:
    drawing::Drawing drawing;
    ObjectId activeSheet;   ///< the sheet shown and edited; the first one when not set

    /// The sheet shown (never null: a drawing always has one).
    drawing::Sheet&       Sheet();
    const drawing::Sheet& Sheet() const;
    /// Its number, from 1.
    int SheetNumber() const;

    // --- changes and undo -----------------------------------------------------
    /// One undoable change: the drawing as it was is kept, then `change` runs.
    void Change(std::string label, const std::function<void(drawing::Drawing&)>& change);
    bool CanUndo() const { return !myUndo.empty(); }
    bool CanRedo() const { return !myRedo.empty(); }
    std::string UndoLabel() const { return myUndo.empty() ? std::string{} : myUndo.back().label; }
    std::string RedoLabel() const { return myRedo.empty() ? std::string{} : myRedo.back().label; }
    void Undo();
    void Redo();
    /// Counts every change (autosave, «изменён»).
    std::uint64_t Revision() const { return myRevision; }
    bool IsModified() const { return myRevision != mySavedRevision; }
    void MarkSaved() { mySavedRevision = myRevision; }

    /// The file the model paths are relative to (the drawing's own file);
    /// empty before the first save — then they are full paths.
    std::filesystem::path savedAt;

    /// The page shown of a specification sheet (it may take several), from 0.
    int activePage = 0;
    /// The view picked on the sheet or in the tree (not saved).
    ObjectId selectedView;
    /// The dimension picked (not saved); a view and a dimension are not picked together.
    ObjectId selectedAnnotation;
    /// The lines of the views, computed from the models (app/drawing/DrawingViews).
    std::shared_ptr<DrawingViews> views;

    // --- the camera of the sheet ------------------------------------------------
    Vec2   center;          ///< sheet point in the middle of the canvas, mm
    double zoom = 0;        ///< pixels per mm; 0 — fit the sheet on the next frame

    /// Model paths for a drawing written to `target` (was at `from`, empty
    /// before its first save): relative to the new folder, '/' between folders.
    /// A path as the drawing keeps it: UTF-8, '/' between folders on every system.
    static std::string StoredPath(const std::filesystem::path& path);
    static void RebasePaths(drawing::Drawing& d, const std::filesystem::path& from, const std::filesystem::path& target);

private:
    struct Step {
        std::string label;
        std::string json;   ///< the drawing before (undo) or after (redo) the step
    };
    void Restore(const std::string& json);
    std::vector<Step> myUndo, myRedo;
    std::uint64_t myRevision = 0, mySavedRevision = 0;
};

} // namespace bz::app
