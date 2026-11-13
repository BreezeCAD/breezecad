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

    /// The bodies of the model now, copied once per its state; empty and
    /// `problem` when there are none. `key` — that state.
    kernel::ViewShapes Shapes(Workspace& ws, DrawingState& d, ObjectId model, std::string* key = nullptr, std::string* problem = nullptr);
    /// The model edges of a dimension of `view` now (Dimension.h); cached
    /// while the model and the references are the same.
    std::vector<std::optional<drawing::ModelEdge>> Edges(Workspace& ws, DrawingState& d, const drawing::View& view,
                                                         const std::vector<drawing::Json>& refs);
    /// The model edge under a sheet point of `view`, within `tolerance` mm of
    /// the sheet; its reference as JSON in `ref`.
    std::optional<drawing::ModelEdge> EdgeAt(Workspace& ws, DrawingState& d, const drawing::View& view, Vec2 sheetPoint,
                                             double tolerance, drawing::Json& ref);

    /// The model's box (placing new views); loads it if it is not open.
    std::optional<kernel::Bounds> ModelBounds(Workspace& ws, DrawingState& d, ObjectId model, std::string* problem = nullptr);

    /// The file of a model of the drawing: its path made absolute.
    static std::filesystem::path ModelFile(const DrawingState& d, ObjectId model);

    /// The title block fields the model of the sheet gives (as Inventor's
    /// iProperties): name, designation, material, mass, scale. A field the
    /// user filled in wins (drawing::LayoutSheet). An assembly drawing's
    /// designation gets «СБ» (ГОСТ 2.102), its specification's does not.
    drawing::Fields AutoFields(Workspace& ws, DrawingState& d, const drawing::Sheet& sheet);
    /// The rows of a specification sheet from its assembly; none without one.
    std::vector<drawing::SpecRow> SpecRows(Workspace& ws, DrawingState& d, const drawing::Sheet& sheet);
    /// All of it for laying the sheets out (the screen, PDF).
    drawing::SheetContent Content(Workspace& ws, DrawingState& d);
    /// The model whose properties the sheet shows: a specification's
    /// assembly, else the model of the sheet's first view, else the drawing's first.
    static ObjectId ModelOfSheet(const DrawingState& d, const drawing::Sheet& sheet);

private:
    struct Source;
    struct Entry {
        std::string        key;     ///< what the lines were computed for: model, revision, direction
        drawing::ViewLines lines;
        bool               ready = false;
        std::string        problem;
    };
    /// The model of `id` now: an open tab's or one read here; nullptr and `problem` if none.
    Session* SessionOf(Workspace& ws, DrawingState& d, ObjectId id, std::string& sourceKey, std::string& problem);
    void Start(const drawing::View& view, const std::string& key, const kernel::ViewShapes& shapes);

    std::map<std::string, std::unique_ptr<Source>> mySources;   ///< by file
    std::map<std::string, std::pair<std::string, kernel::ViewShapes>> myShapes;   ///< by model: its state and its bodies then
    std::map<ObjectId, Entry>                      myEntries;
    struct Found {
        std::string                                    key;
        std::vector<std::optional<drawing::ModelEdge>> edges;
    };
    std::map<std::string, Found> myFound;   ///< by the references' text
    /// Fields and rows by model, kept while the model's state (`key`) is the same.
    struct Properties {
        std::string                   key;
        drawing::Fields               fields;   ///< name, designation, material, mass
        bool                          assembly = false;
        std::vector<drawing::SpecRow> rows;
        std::string                   rowsKey;
    };
    std::map<ObjectId, Properties> myProperties;
    std::future<Expected<kernel::ProjectedLines>> myJob;
    ObjectId                                      myJobView;
    std::string                                   myJobKey;
};

} // namespace bz::app
