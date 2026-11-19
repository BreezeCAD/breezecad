#include "app/sketch/SketchRepair.h"

#include "app/AppContext.h"
#include "app/Session.h"
#include "core/I18n.h"
#include "core/Monitor.h"
#include "document/Commands.h"
#include "document/SketchFeature.h"
#include "ui/Menus.h"
#include "ui/Ui.h"
#include "ui/Widgets.h"

#include <imgui.h>

namespace bz::app {

bool RestoreSketchBeforeBreak(Session& session, ObjectId sketch) {
    const auto* current = session.doc.FindAs<doc::SketchFeature>(sketch);
    if (!current) return false;
    std::unique_ptr<doc::Feature> edited = current->Clone();
    auto* sk = static_cast<doc::SketchFeature*>(edited.get());
    if (!sk->RestoreBeforeBreak(session.doc.Ids())) return false;
    mon::Action("sketch", "восстановление эскиза: " + current->name);
    session.Execute(doc::ReplaceFeature(std::move(edited), N_("Восстановление эскиза")));
    return true;
}

bool RestoreSketchWithNotice(AppContext& app, Session& session, ObjectId sketch) {
    const auto* current = session.doc.FindAs<doc::SketchFeature>(sketch);
    if (!current) return false;
    const std::string name = current->name;
    if (!RestoreSketchBeforeBreak(session, sketch)) return false;
    ShowRestoreNotice(app, session, name, N_("Восстановление эскиза"));
    return true;
}

bool RestoreNotice::CanUndo() const {
    return active && scope && scope->CanUndo() && (undoLabel.empty() || scope->UndoLabel() == undoLabel);
}

void RestoreNotice::Undo() {
    if (Session* s = scope; s && s->CanUndo()) s->Undo();
    Dismiss();
}

void ShowRestoreNotice(AppContext& app, Session& scope, std::string sketch, std::string undoLabel) {
    app.shell.restoreNotice = RestoreNotice{&scope, std::move(sketch), std::move(undoLabel), true};
}

void DrawSketchRepairBanner(AppContext& app, Session& session, const ViewportFrame& vf) {
    if (session.mode != EditMode::Sketch || !session.activeSketch) return;
    const auto* sk = session.doc.FindAs<doc::SketchFeature>(session.activeSketch);
    if (!sk || !sk->HasBrokenProjections()) return;
    ui::Ui& ui = app.ui;
    const ui::ThemeColors& c = ui.C();
    const char* text = Tr("Эскиз сломан из-за изменения источника.");
    const char* restore = Tr("Вернуть как было");
    const char* more = Tr("Подробнее");
    const float fs = ui.S(13), h = ui.S(38), pad = ui.S(12), gap = ui.S(10);
    const float textW = ui.fonts.medium->CalcTextSizeA(fs, FLT_MAX, 0, text).x;
    const auto bw = [&](const char* label) { return ui.fonts.medium->CalcTextSizeA(fs, FLT_MAX, 0, label).x + ui.S(24); };
    const float bw1 = bw(restore), bw2 = bw(more);
    const float w = pad + ui.S(16) + ui.S(8) + textW + gap + bw1 + gap + bw2 + pad;
    const float top = vf.promptTop > 0 ? vf.promptTop : vf.min.y + ui.S(14);
    const ImVec2 p{(vf.min.x + vf.max.x - w) / 2, top};
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled({p.x, p.y + ui.S(3)}, {p.x + w, p.y + h + ui.S(3)}, ui::ToU32(Color{0, 0, 0, 0.12f}), ui.S(10));
    dl->AddRectFilled(p, {p.x + w, p.y + h}, ui::ToU32(c.panel.WithAlpha(0.96f)), ui.S(10));
    dl->AddRect(p, {p.x + w, p.y + h}, ui::ToU32(c.warning.WithAlpha(0.55f)), ui.S(10));
    ui.icons.Draw(dl, "status-warning", {p.x + pad, p.y + (h - ui.S(16)) / 2}, ui.S(16), c.warning);
    dl->AddText(ui.fonts.medium, fs, {p.x + pad + ui.S(24), p.y + (h - fs) / 2 - ui.S(1)}, ui::ToU32(c.text), text);
    ImGui::SetCursorScreenPos({p.x + pad + ui.S(24) + textW + gap, p.y + (h - ui.S(26)) / 2});
    if (ui::SecondaryButton(ui, "##sk-restore", restore, nullptr, {bw1, ui.S(26)})) {
        const ObjectId id = sk->id;
        app.Later([&app, &session, id] { RestoreSketchWithNotice(app, session, id); });
    }
    ImGui::SameLine(0, gap);
    if (ui::SecondaryButton(ui, "##sk-more", more, nullptr, {bw2, ui.S(26)})) ImGui::OpenPopup("##sk-repair-more");
    // «Подробнее»: the same explanation the "!" in the status bar gives.
    if (ImGui::BeginPopup("##sk-repair-more")) {
        ImGui::PushTextWrapPos(ui.S(340));
        ImGui::PushStyleColor(ImGuiCol_Text, ui::ToVec4(c.textSecondary));
        ImGui::TextUnformatted(Tr("Проекция потеряла своё ребро или не сходится с эскизом"));
        ImGui::TextUnformatted(Tr("«Вернуть как было» возвращает эскиз к последнему рабочему виду; спроецированная геометрия станет независимой"));
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
        ImGui::EndPopup();
    }
}

void SketchRepairMenu(AppContext& app, Session& session, ObjectId sketch) {
    ui::Ui& ui = app.ui;
    ui::MenuStyle style(ui);   // before Begin: must outlive the End call
    if (!ImGui::BeginPopup(kSketchRepairPopup)) return;
    const auto* sk = session.doc.FindAs<doc::SketchFeature>(sketch);
    if (sk) {
        if (sk->HasBrokenProjections())
            ui::MenuItem(ui, Tr("Проекция потеряла своё ребро или не сходится с эскизом"), nullptr, "status-warning", false, false);
        else if (!sk->status.message.empty())
            ui::MenuItem(ui, Tr(sk->status.message).c_str(), nullptr, "status-warning", false, false);
        const bool can = !sk->projections.empty() || !sk->contextProjections.empty();
        if (ui::MenuItem(ui, Tr("Восстановить эскиз как до изменения"), nullptr, "ui-undo", false, can)) {
            app.Later([&app, &session, sketch] { RestoreSketchWithNotice(app, session, sketch); });
            ImGui::CloseCurrentPopup();
        }
        // What it costs, under the item: said before it is chosen.
        ImGui::PushStyleColor(ImGuiCol_Text, ui::ToVec4(ui.C().textMuted));
        ImGui::TextUnformatted(Tr("Спроецированная геометрия станет независимой"));
        ImGui::PopStyleColor();
    }
    ImGui::EndPopup();
}

} // namespace bz::app
