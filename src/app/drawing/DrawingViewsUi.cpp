#include "app/drawing/DrawingViewsUi.h"

#include "app/Actions.h"
#include "app/Session.h"
#include "app/Workspace.h"
#include "app/drawing/DrawingViews.h"
#include "core/I18n.h"
#include "drawing/ViewLayout.h"
#include "ui/Menus.h"
#include "ui/Ui.h"

#include <algorithm>
#include <format>

namespace bz::app {

namespace {

namespace dw = bz::drawing;

constexpr const char* kViewMenu = "##drawing-view-menu";
constexpr float kPickMargin = 6;   // px around a view's lines where it is still picked

DrawingState* ActiveDrawing(AppContext& app) {
    Session* s = app.Doc();
    return s && !app.workspace.ShowsHome() ? s->drawing.get() : nullptr;
}

const dw::PlacedView* Find(const std::vector<dw::PlacedView>& placed, ObjectId id) {
    for (const dw::PlacedView& p : placed)
        if (p.view.id == id) return &p;
    return nullptr;
}

/// The view under `at` (sheet mm): the one drawn last wins.
const dw::PlacedView* ViewAt(const std::vector<dw::PlacedView>& placed, Vec2 at, double margin) {
    for (auto it = placed.rbegin(); it != placed.rend(); ++it)
        if (at.x >= it->box.min.x - margin && at.x <= it->box.max.x + margin && at.y >= it->box.min.y - margin &&
            at.y <= it->box.max.y + margin)
            return &*it;
    return nullptr;
}

std::string ViewTitle(const dw::PlacedView& p) {
    const std::string name = Tr(dw::ProjectionName(p.view.projection));
    // The scale is the main view's; the views projected from it share it.
    return p.view.parent.IsValid() ? name : std::format("{} ({})", name, dw::ScaleText(p.scale));
}

/// The menu of the picked view: scale, hidden lines, delete.
void ViewMenu(AppContext& app, DrawingState& d, DrawingViewsState& st) {
    ui::Ui& ui = app.ui;
    const ui::MenuStyle style(ui);
    if (!ImGui::BeginPopup(kViewMenu)) return;
    const dw::PlacedView* picked = Find(PlacedViews(d), st.selected);
    if (!picked) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    const ObjectId id = picked->view.id;
    const dw::Scale current = picked->scale;
    const bool hidden = picked->view.hiddenLines;
    // A change rebuilds the list `picked` points into: nothing of it is used after one.
    if (ui::BeginSubmenu(ui, Tr("Масштаб"), "view-fit")) {
        for (const dw::Scale scale : dw::StandardScales()) {
            if (scale.Factor() > 10 || scale.Factor() < 0.01) continue;   // the rare ones are not offered
            if (ui::MenuItem(ui, dw::ScaleText(scale).c_str(), nullptr, {}, scale == current) && !(scale == current)) SetViewScale(d, id, scale);
        }
        ImGui::EndMenu();
    }
    if (ui::MenuItem(ui, Tr("Невидимые линии"), nullptr, {}, hidden)) SetViewHiddenLines(d, id, !hidden);
    if (ui::MenuItem(ui, Tr("Удалить вид"), "Del", "ui-trash")) RemoveView(d, id);
    ImGui::EndPopup();
}

} // namespace

void RegisterDrawingViewActions(AppContext& app) {
    ActionRegistry& a = app.actions;
    a.Add({"drawing.standardViews", N_("Стандартные виды"), "ds-hidden-lines", 0,
           N_("Главный вид, вид сверху и вид слева детали чертежа — в проекционной связи (ГОСТ 2.305), с линиями невидимого контура"),
           [&app] { return ActiveDrawing(app) != nullptr; },
           [&app] {
               DrawingState* d = ActiveDrawing(app);
               const Expected<ObjectId> main = AddStandardViews(*d, &app.workspace.sessions);
               if (!main) {
                   app.notes.Error(Tr(main.GetError().message));
                   return;
               }
               ViewsOf(*d).selected = *main;
           },
           0, N_("вид проекция главный сверху слева")});
    a.Add({"drawing.removeView", N_("Удалить вид"), "ui-trash", 0, N_("Убрать выбранный вид с листа вместе с проекционными видами от него"),
           [&app] {
               DrawingState* d = ActiveDrawing(app);
               return d && d->views && d->views->selected.IsValid();
           },
           [&app] {
               DrawingState* d = ActiveDrawing(app);
               RemoveView(*d, ViewsOf(*d).selected);
           },
           0, {}});
}

bool SheetViewsInput(AppContext& app, Session& session, const SheetCamera& camera, bool hovered) {
    DrawingState& d = *session.drawing;
    DrawingViewsState& st = ViewsOf(d);
    st.cache.Refresh(d, &app.workspace.sessions);

    const ImGuiIO& io = ImGui::GetIO();
    const Vec2 mouse = camera.ToSheet(io.MousePos);
    {
        const std::vector<dw::PlacedView>& placed = PlacedViews(d);
        if (st.selected.IsValid() && !Find(placed, st.selected)) st.selected = {};   // undone, or on another sheet
        const dw::PlacedView* under = hovered ? ViewAt(placed, mouse, kPickMargin / camera.zoom) : nullptr;
        st.hovered = under ? under->view.id : ObjectId{};

        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            st.selected = st.hovered;
            if (under) {
                st.dragged    = under->view.id;
                st.dragMoved  = false;
                st.dragFrom   = mouse;
                st.dragOrigin = st.dragAt = under->view.at;
            }
        }
        // The right button without a drag: the view's menu (a drag moves the sheet, as before).
        if (hovered && under && ImGui::IsMouseReleased(ImGuiMouseButton_Right) &&
            io.MouseDragMaxDistanceSqr[ImGuiMouseButton_Right] < io.MouseDragThreshold * io.MouseDragThreshold) {
            st.selected = under->view.id;
            ImGui::OpenPopup(kViewMenu);
        }
    }

    if (st.dragged.IsValid()) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) st.dragMoved = true;
            if (st.dragMoved) {
                const dw::Item* item = dw::FindViewItem(d.Sheet(), st.dragged);
                if (const std::optional<dw::ModelView> view = item ? dw::ReadModelView(*item) : std::nullopt)
                    st.dragAt = dw::KeepLink(d.Sheet(), *view, st.dragOrigin + (mouse - st.dragFrom));
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
            }
        } else {
            const ObjectId id = st.dragged;
            const bool moved = st.dragMoved && !(st.dragAt == st.dragOrigin);
            st.dragged   = {};
            st.dragMoved = false;
            if (moved) MoveView(d, id, st.dragAt);
        }
    }

    if (hovered && st.selected.IsValid() && !io.WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_Delete, false)) RemoveView(d, st.selected);
        else if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) st.selected = {};
    }
    ViewMenu(app, d, st);
    return st.dragged.IsValid();
}

void DrawSheetViews(AppContext& app, Session& session, ImDrawList* dl, const SheetCamera& camera) {
    ui::Ui& ui = app.ui;
    const ui::ThemeColors& c = ui.C();
    DrawingState& d = *session.drawing;
    DrawingViewsState& st = ViewsOf(d);
    const ImU32 ink = ui::ToU32(c.sheetInk);
    // The same pens as the frame (ГОСТ 2.303): the main line 0.5 mm, the thin one 0.18; a pixel at least.
    const float thick = std::max(1.0f, static_cast<float>(0.5 * camera.zoom));
    const float thin  = std::max(1.0f, static_cast<float>(0.18 * camera.zoom));
    std::vector<ImVec2> points;
    for (const dw::PlacedView& p : PlacedViews(d)) {
        const ImVec2 a = camera.ToScreen({p.box.min.x, p.box.max.y}), b = camera.ToScreen({p.box.max.x, p.box.min.y});
        for (const dw::ViewPath& path : p.paths) {
            points.clear();
            for (const Vec2 at : path.points) points.push_back(camera.ToScreen(at));
            dl->AddPolyline(points.data(), static_cast<int>(points.size()), ink, ImDrawFlags_None,
                            path.weight == dw::LineWeight::Thick ? thick : thin);
        }
        if (!p.computed) {
            // No lines: the place of the view and why it is empty.
            dl->AddRect(a, b, ui::ToU32(c.danger), 0, ImDrawFlags_None, 1.0f);
            const std::string why = Tr(st.cache.Problem(p.view.model));
            const std::string text = why.empty() ? Tr(dw::ProjectionName(p.view.projection)) : why;
            const ImVec2 size = ui.fonts.body->CalcTextSizeA(ui.S(12), FLT_MAX, b.x - a.x - ui.S(8), text.c_str());
            dl->AddText(ui.fonts.body, ui.S(12), {a.x + ui.S(4), (a.y + b.y - size.y) / 2}, ui::ToU32(c.danger), text.c_str(), nullptr,
                        b.x - a.x - ui.S(8));
        }
        const bool selected = p.view.id == st.selected;
        if (selected || p.view.id == st.hovered) {
            const float m = ui.S(4);
            dl->AddRect({a.x - m, a.y - m}, {b.x + m, b.y + m}, ui::ToU32(selected ? c.accent : c.accentSoftBorder), 0, ImDrawFlags_None,
                        selected ? 1.5f : 1.0f);
        }
    }
}

std::string SheetViewsNote(DrawingState& d) {
    DrawingViewsState& st = ViewsOf(d);
    const std::vector<dw::PlacedView>& placed = PlacedViews(d);
    if (placed.empty())
        return Tr("«Стандартные виды» на вкладке «Виды» — главный вид, сверху и слева. Основная надпись — двойной щелчок по ней");
    for (const dw::PlacedView& p : placed)
        if (!p.computed) {
            const std::string why = st.cache.Problem(p.view.model);
            if (!why.empty()) return Tr(why);
        }
    return Tr("Вид перетаскивается мышью; правая кнопка — масштаб и невидимые линии. Виды следуют за деталью");
}

std::vector<ViewRow> SheetViewRows(DrawingState& d) {
    std::vector<ViewRow> rows;
    const ObjectId selected = ViewsOf(d).selected;
    for (const dw::PlacedView& p : PlacedViews(d)) rows.push_back({p.view.id, ViewTitle(p), p.view.id == selected});
    return rows;
}

void SelectView(DrawingState& d, ObjectId view) { ViewsOf(d).selected = view; }

} // namespace bz::app
