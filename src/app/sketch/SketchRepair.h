#pragma once
// A sketch whose projections broke (their edge lost, or the sketch no longer
// agreeing with them after a change above it): the "!" at the sketch, in the
// tree and in the status bar while it is edited, offers to bring the sketch
// back as it was before and unlink its projections from the model.

#include "core/Id.h"

#include <string>

namespace bz::app {

struct AppContext;
class Session;
struct ViewportFrame;   // app/AppContext.h

/// The panel after a successful restore: «Эскиз восстановлен. [Отменить]
/// [Повторить изменение источника]». `scope` is weak: the UI checks the
/// document is still open before showing or clicking anything.
struct RestoreNotice {
    Session*    scope = nullptr;   // the document the restore was undone in
    std::string sketch;            // its name, for the message
    std::string undoLabel;         // the step «Отменить» reverts (empty: any)
    bool        active = false;

    /// «Отменить» is offered honestly: the step it reverts is still the
    /// document's last one (nothing was done on top of the restore).
    bool CanUndo() const;
    /// The undo itself; the caller made sure `scope` is still open. Clears
    /// the notice; safe to call when there is nothing to undo.
    void Undo();
    void Dismiss() { *this = RestoreNotice{}; }
};

/// Fills the notice (app.shell.restoreNotice) after a successful restore.
void ShowRestoreNotice(AppContext& app, Session& scope, std::string sketch, std::string undoLabel);
/// Draws the panel while its document is open and its undo step is the last
/// one; dismisses it otherwise. Returns true while it wants another frame.
bool DrawRestoreNotice(AppContext& app);

/// «Восстановить эскиз как до изменения»: one undoable step in the part.
/// False when the sketch is not there or has no projections.
bool RestoreSketchBeforeBreak(Session& session, ObjectId sketch);
/// The same, and the notice panel on success (what buttons call).
bool RestoreSketchWithNotice(AppContext& app, Session& session, ObjectId sketch);

/// The compact banner at the top of the view while a broken sketch is edited
/// («Эскиз сломан из-за изменения источника. [Вернуть как было] [Подробнее]»).
void DrawSketchRepairBanner(AppContext& app, Session& session, const ViewportFrame& vf);

/// The menu the "!" opens (ImGui::OpenPopup(kSketchRepairPopup) in the same
/// ID scope first). Draws nothing while it is closed.
inline constexpr const char* kSketchRepairPopup = "##fix-sketch";
void SketchRepairMenu(AppContext& app, Session& session, ObjectId sketch);

} // namespace bz::app
