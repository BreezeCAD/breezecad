#pragma once

#include "app/AssemblyState.h"
#include "app/drawing/DrawingState.h"
#include "app/EditMode.h"
#include "core/Expected.h"
#include "document/Commands.h"
#include "document/Document.h"
#include "kernel/Model.h"
#include "render/Viewport.h"

#include <functional>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace bz::app {

struct ImportJob;   // app/Import.cpp

/// One open document with everything that belongs to it: the recipe,
/// undo history, computed geometry, file, camera and editing state.
class Session {
public:
    Session();
    ~Session();   // waits for a rebuild still running on another thread
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    doc::Document  doc;
    doc::History   history;
    kernel::Model  model;

    /// Set for an assembly: `doc` then stays empty and `model` shows the
    /// placed components (one body per component, body id = component id).
    std::unique_ptr<AssemblyState> assembly;
    bool IsAssembly() const { return assembly != nullptr; }

    /// Set for a drawing (ADR-0012): `doc` and `model` stay empty; the
    /// editor (app/drawing) shows the sheet instead of the 3D view.
    std::unique_ptr<DrawingState> drawing;
    bool IsDrawing() const { return drawing != nullptr; }
    /// Makes this session a drawing (a new one: one A3 sheet).
    DrawingState& MakeDrawing();

    /// A part edited inside an assembly (AssemblyState::parts): that
    /// assembly's session. It is saved with the assembly, not by itself.
    Session* host = nullptr;
    /// While edited in place: the component of `host` it is edited as.
    ObjectId inPlaceOf;
    /// Makes this session an assembly (an empty one).
    AssemblyState& MakeAssembly();
    /// The kind of document (doc::DocumentTypes): "part", "assembly" or "drawing".
    std::string_view DocumentType() const;

    /// Counts every change of the content (autosave).
    std::uint64_t ContentRevision() const;
    /// Writes the document to `target` (part or assembly). An assembly
    /// written elsewhere than its own file keeps pointing at the same
    /// parts (its relative paths are adjusted in the copy).
    /// `preview`: the model as a PNG for the file manager (empty: the file keeps its old one).
    Status WriteFile(const std::filesystem::path& target, std::string_view generator,
                     std::span<const std::uint8_t> preview = {}) const;

    /// «Импорт»: the file being read into this part on another thread, or
    /// the card saying why it could not be (app/Import.h).
    std::shared_ptr<ImportJob> importing;

    std::filesystem::path path;        ///< empty until saved once
    std::string           untitledName; ///< "Деталь1" for documents never saved
    std::string           autosaveKey;  ///< folder name for autosave copies

    render::CameraState camera;
    bool                hasCamera = false;

    EditMode mode = EditMode::Model;
    ObjectId activeSketch;      ///< sketch being edited in EditMode::Sketch
    ObjectId selectedFeature;   ///< selection in the tree
    /// Faces chosen in the view (part, Model mode): what the context menu
    /// acts on. Kept by reference, so they follow the part when it changes.
    std::vector<render::SelectedFace> selectedFaces;

    // --- autosave bookkeeping ---
    double        unsavedSince      = -1.0;   ///< time of the first unsaved change, -1 = clean
    /// A failure was recovered from since the last save (app/Recovery): the
    /// «Сохранить изменения?» question says so, not to save a broken state
    /// unawares (BETA-BUGS, «Кубик монолит», 12.11.2026).
    bool          recoveredSinceSave = false;
    std::uint64_t autosavedRevision = 0;

    /// File name without extension, or the untitled name.
    std::string Title() const;
    /// How parts edited in this assembly name it in their projections from
    /// other parts (doc::ContextProjection::assembly): its file name without
    /// extension, the untitled name before the first save.
    std::string ContextName() const;
    /// An assembly counts the parts edited in it too.
    bool IsModified() const;
    bool CanUndo() const { return drawing ? drawing->CanUndo() : assembly ? assembly->CanUndo() : history.CanUndo(); }
    bool CanRedo() const { return drawing ? drawing->CanRedo() : assembly ? assembly->CanRedo() : history.CanRedo(); }
    std::string UndoLabel() const { return drawing ? drawing->UndoLabel() : assembly ? assembly->UndoLabel() : history.UndoLabel(); }
    std::string RedoLabel() const { return drawing ? drawing->RedoLabel() : assembly ? assembly->RedoLabel() : history.RedoLabel(); }
    /// After saving to `path` (the session's file from now on).
    void MarkSaved();

    /// Runs an undoable command.
    void Execute(std::unique_ptr<doc::Command> command);
    void Undo();
    void Redo();

    /// Commits an edit that was already applied to feature `before->id` in
    /// place (live preview, dragging): the edit becomes one undo step that
    /// restores `before`.
    void CommitInPlaceEdit(std::unique_ptr<doc::Feature> before, std::string label);

    /// Rebuilds geometry if the document changed since the last rebuild.
    /// Returns true when it did.
    bool RebuildIfNeeded();
    /// The same, to the end at once (before reading the result: a commit,
    /// an export).
    bool RebuildNow();
    /// RebuildNow for a caller that can keep the window alive meanwhile (a
    /// command from a module): a long rebuild runs on another thread, as
    /// one started by hand does, and `alive` is called while it runs (it
    /// draws the window; false — the user asked to cancel). False when the
    /// rebuild was cancelled: its result is dropped, the model is as it was
    /// and the document is the caller's to put back. Without `alive` it is
    /// RebuildNow.
    bool RebuildWaiting(const std::function<bool()>& alive);
    /// What that wait is for, shown next to the progress (the command's name).
    const std::string& WaitingFor() const { return myWaitingFor; }
    void SetWaitingFor(std::string what) { myWaitingFor = std::move(what); }
    /// A rebuild is under way, in slices or on another thread (the window
    /// shows its progress).
    bool Rebuilding() const { return myRebuilding; }
    /// The rebuild under way runs on another thread (the window only has
    /// to show that it is busy).
    bool RebuildingInBackground() const { return myBackground != nullptr; }
    /// How far that rebuild is.
    kernel::Model::Progress RebuildProgress() const;

    /// Display quality (Settings::graphics): a rebuild on another thread
    /// meshes its bodies for it there, not in the window's thread.
    void SetMeshQuality(int level) { myMeshQuality = level; }
    /// Long rebuilds may run on another thread (Settings::backgroundRebuild).
    void AllowBackground(bool allow) { myBackgroundAllowed = allow; }

    /// While a sketch or an operation is edited: rebuild only up to it
    /// (`last`, a feature index); the rest follows when editing ends.
    /// `showAtLast`: show the part right after it (an operation's preview).
    void LimitRebuild(std::optional<std::size_t> last, bool showAtLast) {
        myRebuildLimit = last;
        myShowAtLimit = showAtLast;
    }

private:
    struct Background;
    /// A long rebuild runs on another thread, on copies of the document
    /// and of the shapes it starts from; the window keeps showing the part
    /// as it was until it is done.
    std::unique_ptr<Background> myBackground;
    void StartBackground(std::size_t limit);
    /// Waits for the background rebuild and takes its result. True when
    /// it was made for the document as it is now.
    bool FinishBackground();
    /// FinishBackground and what follows it in the session's own books.
    void AdoptBackground();
    std::string myWaitingFor;

    std::optional<std::size_t> myRebuildLimit;
    bool                       myRebuilding = false;
    bool                       Rebuild(double budgetMs);
    bool                       myShowAtLimit = false;
    int                        myMeshQuality = 1;
    bool                       myBackgroundAllowed = true;
    std::uint64_t myBuiltRevision = static_cast<std::uint64_t>(-1);
    std::size_t   myBuiltFeatureCount = 0;   ///< operations in the document when the model was last built
    double        myLastRebuildSeconds = 0;   ///< how long the last rebuild took
    double        myLastRebuildAt = 0;        ///< when it finished (ImGui time)
    double        myRebuildBeganAt = 0;       ///< first slice of the rebuild under way (monitor)
    double        myRebuildWorkMs = 0;        ///< its slices together (monitor)
};

} // namespace bz::app
