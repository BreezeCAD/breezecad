#pragma once
// «Кадр»: a picture of the part for posts and the site (the author approved
// the mock-up, branch design/photo-mode). A panel on the left like the
// operations; the view shows the frame the picture holds. Nothing in the
// document changes.

#include "app/features/OperationEditor.h"
#include "core/I18n.h"

#include <filesystem>

namespace bz::app {

class PhotoEditor final : public OperationEditor {
public:
    void Start(AppContext& app, Session& session);

    bool        IsOpen() const override { return myOpen; }
    ObjectId    EditedFeature() const override { return {}; }
    bool        CanEdit(const doc::Feature&) const override { return false; }
    void        EditExisting(AppContext&, Session&, ObjectId) override {}
    void        Apply(AppContext& app, Session& session) override { Save(app, session); }
    bool        HasApply() const override { return false; }
    void        Done(AppContext& app, Session& session) override { Cancel(app, session); }
    void        Cancel(AppContext& app, Session& session) override;
    void        Drop(AppContext& app) override;
    /// Esc while the picture is being made: stops it, the panel stays.
    bool        StepBack(AppContext& app, Session& session) override;
    void        DrawPanel(AppContext& app, Session& session) override;
    void        DrawInViewport(AppContext& app, Session& session, const ViewportFrame& vf) override;
    const char* Title() const override { return Tr("Кадр"); }
    const char* Icon() const override { return "view-render"; }
    std::string Hint() const override;

    /// The picture's width / height for the size chosen.
    double Aspect() const;

    /// A picture under way (the status bar shows how far).
    bool   Rendering() const { return myRendering; }
    double Progress(const AppContext& app) const;

private:
    void Save(AppContext& app, Session& session);
    /// Each frame while rendering: the picture done → written to the file.
    void Poll(AppContext& app);

    bool myOpen = false;
    int  myBackground = 0;   ///< PhotoOptions::Background
    bool myFloor = true;
    bool myEdges = true;
    int  mySize = 1;         ///< kSizes
    std::filesystem::path myFolder;   ///< where the last picture went
    bool                  myRendering = false;
    std::filesystem::path myTarget;   ///< where the picture being made goes
};

} // namespace bz::app
