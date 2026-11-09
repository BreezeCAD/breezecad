#pragma once
// The lines of a drawing's views (ADR-0012, step 3), computed from its models
// and kept until a model changes. A model open in a tab is used as it is
// now — edit the part and its views follow; a closed one is read from its
// file (again when the file changes). The lines are computed in the
// background, one view at a time; the sheet shows each when it is ready.

#include "core/Id.h"
#include "drawing/DrawingView.h"
#include "drawing/SheetLayout.h"
#include "kernel/Model.h"
#include "kernel/drawing/HiddenLines.h"

#include <filesystem>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <string>

namespace bz::app {

class DrawingState;
class Session;
class Workspace;

class DrawingViews {
public:
    DrawingViews() = default;
    ~DrawingViews();
    DrawingViews(const DrawingViews&) = delete;
    DrawingViews& operator=(const DrawingViews&) = delete;

    /// The cache of `d` (made on first use).
    static DrawingViews& Of(DrawingState& d);

    /// The view's lines, or nullptr while they are computed or cannot be
    /// (then Problem() says why). Starts what is out of date.
    const drawing::ViewLines* Lines(Workspace& ws, DrawingState& d, const drawing::View& view);
    /// Why the view has no lines now; empty when it has them.
    std::string Problem(ObjectId view) const;
    /// Some view is being computed.
    bool Busy() const { return myJob.valid(); }
    /// Takes a finished computation; call once a frame.
    void Poll();
    /// Every view of the drawing computed, waiting for it (PDF export).
    void Finish(Workspace& ws, DrawingState& d);

    /// The model's box (placing new views); loads it if it is not open.
    std::optional<kernel::Bounds> ModelBounds(Workspace& ws, DrawingState& d, ObjectId model, std::string* problem = nullptr);

    /// The file of a model of the drawing: its path made absolute.
    static std::filesystem::path ModelFile(const DrawingState& d, ObjectId model);

private:
    struct Source;
    struct Entry {
        std::string        key;     ///< what the lines were computed for: model, revision, direction
        drawing::ViewLines lines;
        bool               ready = false;
        std::string        problem;
    };
    /// The model of `id` now: an open tab's or one read here; nullptr and `problem` if none.
    const kernel::Model* ModelOf(Workspace& ws, DrawingState& d, ObjectId id, std::string& sourceKey, std::string& problem);
    void Start(ObjectId view, const std::string& key, const kernel::ViewShapes& shapes, drawing::ViewDirection direction);

    std::map<std::string, std::unique_ptr<Source>> mySources;   ///< by file
    std::map<std::string, std::pair<std::string, kernel::ViewShapes>> myShapes;   ///< by model: its state and its bodies then
    std::map<ObjectId, Entry>                      myEntries;
    std::future<Expected<kernel::ProjectedLines>> myJob;
    ObjectId                                      myJobView;
    std::string                                   myJobKey;
};

} // namespace bz::app
