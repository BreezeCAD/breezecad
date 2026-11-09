#include "app/drawing/DrawingEditor.h"

#include "app/Actions.h"
#include "app/Operations.h"
#include "app/Session.h"
#include "app/Workspace.h"
#include "app/drawing/DrawingViews.h"
#include "app/shell/Shell.h"
#include "core/I18n.h"
#include "core/PathText.h"
#include "drawing/DrawingPdf.h"
#include "drawing/DrawingView.h"
#include "drawing/SheetLayout.h"
#include "drawing/ViewPlacement.h"
#include "platform/FileDialogs.h"
#include "platform/Paths.h"
#include "ui/Ui.h"
#include "ui/Widgets.h"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace bz::app {

namespace {

namespace dw = bz::drawing;

constexpr std::array<dw::SheetSize, 5> kSizes{dw::SheetSize::A4, dw::SheetSize::A3, dw::SheetSize::A2, dw::SheetSize::A1,
                                              dw::SheetSize::A0};

/// The «Лист и основная надпись» window: a copy of the sheet edited until «ОК».
struct SheetDialog {
    bool      open = false;
    bool      justOpened = false;
    ObjectId  sheet;
    dw::Sheet edit;
};

SheetDialog& Dialog() {
    static SheetDialog dialog;   // one program, one such window
    return dialog;
}

DrawingState* ActiveDrawing(AppContext& app) {
    Session* s = app.Doc();
    return s && !app.workspace.ShowsHome() ? s->drawing.get() : nullptr;
}

void OpenSheetDialog(AppContext& app) {
    DrawingState* d = ActiveDrawing(app);
    if (!d) return;
    SheetDialog& dlg = Dialog();
    dlg.open       = true;
    dlg.justOpened = true;
    dlg.sheet      = d->Sheet().id;
    dlg.edit       = d->Sheet();
}

std::string SizeName(dw::SheetSize size) {
    // Cyrillic «А», as written on drawings.
    return std::format("А{}", std::string(dw::SizeId(size)).substr(1));
}

std::string SheetTitle(const dw::Sheet& s) { return std::format("{} ({})", s.name, SizeName(s.size)); }

// --- the sheet on the screen ----------------------------------------------------

struct SheetView {
    ImVec2 middle;   ///< the canvas centre, px
    Vec2   center;   ///< sheet point there, mm
    double zoom;     ///< px per mm
    double height;   ///< the sheet's height, mm

    ImVec2 ToScreen(Vec2 p) const {
        return {static_cast<float>(middle.x + (p.x - center.x) * zoom), static_cast<float>(middle.y - (p.y - center.y) * zoom)};
    }
    Vec2 ToSheet(ImVec2 p) const { return {center.x + (p.x - middle.x) / zoom, center.y - (p.y - middle.y) / zoom}; }
};

/// A text of the sheet: drawn, then its vertices squeezed to the cell and
/// turned (ImGui draws no turned text).
void DrawLabel(ImDrawList* dl, ImFont* font, const SheetView& v, const dw::Label& label, ImU32 color) {
    if (label.text.empty()) return;
    // ГОСТ 2.304 gives the height of capitals; a font's size is more than that.
    const float size = static_cast<float>(label.height * v.zoom / 0.72);
    if (size < 3.0f) return;   // smaller than a pixel row a letter: not worth drawing
    const ImVec2 anchor = v.ToScreen(label.at);
    const float ascent = font->Ascent * size / font->FontSize;
    const float width  = font->CalcTextSizeA(size, FLT_MAX, 0, label.text.c_str()).x;
    const float limit  = static_cast<float>(label.maxWidth * v.zoom);
    const float squeeze = limit > 0 && width > limit ? limit / width : 1.0f;
    const float shift   = label.align == dw::Align::Center ? -width * squeeze / 2 : 0.0f;
    const int first = dl->VtxBuffer.Size;
    dl->AddText(font, size, {anchor.x, anchor.y - ascent}, color, label.text.c_str());
    const double a = label.rotation * kPi / 180.0;
    const float cs = static_cast<float>(std::cos(a)), sn = static_cast<float>(std::sin(a));
    for (int i = first; i < dl->VtxBuffer.Size; ++i) {
        ImVec2& p = dl->VtxBuffer[i].pos;
        const float dx = (p.x - anchor.x) * squeeze + shift, dy = p.y - anchor.y;
        // Counter-clockwise on the sheet: y goes down on the screen.
        p = {anchor.x + dx * cs + dy * sn, anchor.y - dx * sn + dy * cs};
    }
}

/// A line of a view; dashes as on paper (ГОСТ 2.303), solid when they would
/// be too small to tell on the screen.
void DrawPolyline(ImDrawList* dl, const SheetView& v, const dw::Polyline& p, ImU32 color, float thick, float thin) {
    if (p.points.size() < 2) return;
    const float width = p.weight == dw::LineWeight::Thick ? thick : thin;
    std::vector<ImVec2> points;
    points.reserve(p.points.size());
    for (Vec2 q : p.points) points.push_back(v.ToScreen(q));
    const std::vector<double> dash = dw::DashOf(p.style);
    if (dash.empty() || dash[1] * v.zoom < 2.5) {
        dl->AddPolyline(points.data(), static_cast<int>(points.size()), color, ImDrawFlags_None, width);
        return;
    }
    std::size_t k = 0;
    double left = dash[0] * v.zoom;
    for (std::size_t i = 1; i < points.size(); ++i) {
        const ImVec2 a = points[i - 1], b = points[i];
        const double len = std::hypot(b.x - a.x, b.y - a.y);
        if (len <= 0) continue;
        const ImVec2 dir{static_cast<float>((b.x - a.x) / len), static_cast<float>((b.y - a.y) / len)};
        double at = 0;
        while (at < len) {
            const double step = std::min(left, len - at);
            if (k % 2 == 0) {
                const auto f0 = static_cast<float>(at), f1 = static_cast<float>(at + step);
                dl->AddLine({a.x + dir.x * f0, a.y + dir.y * f0}, {a.x + dir.x * f1, a.y + dir.y * f1}, color, width);
            }
            at += step;
            left -= step;
            if (left <= 1e-6) {
                k = (k + 1) % dash.size();
                left = dash[k] * v.zoom;
            }
        }
    }
}

void DrawSheet(ImDrawList* dl, ui::Ui& ui, const SheetView& v, const dw::SheetLayout& l) {
    const ui::ThemeColors& c = ui.C();
    const ImVec2 a = v.ToScreen({0, l.height}), b = v.ToScreen({l.width, 0});
    const float shadow = ui.S(4);
    dl->AddRectFilled({a.x + shadow, a.y + shadow}, {b.x + shadow, b.y + shadow}, ui::ToU32(c.sheetShadow));
    dl->AddRectFilled(a, b, ui::ToU32(c.sheetPaper));
    const ImU32 ink = ui::ToU32(c.sheetInk);
    // ГОСТ 2.303: the main line 0.5 mm here, a thin one about a third of it; a pixel at least.
    const float thick = std::max(1.0f, static_cast<float>(0.5 * v.zoom));
    const float thin  = std::max(1.0f, static_cast<float>(0.18 * v.zoom));
    for (const dw::Stroke& s : l.strokes)
        dl->AddLine(v.ToScreen(s.a), v.ToScreen(s.b), ink, s.weight == dw::LineWeight::Thick ? thick : thin);
    for (const dw::Polyline& p : l.polylines) DrawPolyline(dl, v, p, ink, thick, thin);
    for (const dw::Label& label : l.labels) DrawLabel(dl, ui.fonts.body, v, label, ink);
}

/// The whole sheet: `margin` around it, `top` more above (the note there).
void FitInto(DrawingState& d, const dw::SheetLayout& l, ImVec2 size, float margin, float top) {
    const double zx = (size.x - 2 * margin) / l.width, zy = (size.y - 2 * margin - top) / l.height;
    d.zoom   = std::max(0.05, std::min(zx, zy));
    // Shifted down by half the note's room: the sheet sits under it.
    d.center = {l.width / 2, l.height / 2 + top / 2 / d.zoom};
}

// --- views ------------------------------------------------------------------------

const char* DirectionName(dw::ViewDirection d) {
    switch (d) {
        case dw::ViewDirection::Front:  return N_("Вид спереди");
        case dw::ViewDirection::Back:   return N_("Вид сзади");
        case dw::ViewDirection::Top:    return N_("Вид сверху");
        case dw::ViewDirection::Bottom: return N_("Вид снизу");
        case dw::ViewDirection::Left:   return N_("Вид слева");
        case dw::ViewDirection::Right:  return N_("Вид справа");
        case dw::ViewDirection::Iso:    return N_("Изометрия");
    }
    return N_("Вид спереди");
}

std::string ViewTitle(const dw::View& v) { return std::format("{} ({})", Tr(DirectionName(v.direction)), dw::ScaleText(v.scale)); }

dw::ViewLinesSource LinesSource(AppContext& app, DrawingState& d) {
    return [&app, &d](const dw::Sheet&, const dw::View& v) { return DrawingViews::Of(d).Lines(app.workspace, d, v); };
}

Box3 BoxOf(const kernel::Bounds& b) {
    Box3 box;
    box.Include(b.min);
    box.Include(b.max);
    return box;
}

/// Where a view is on the sheet: its lines, else its model's box, else a square around its origin.
Box2 BoxOfView(AppContext& app, DrawingState& d, const dw::View& v) {
    DrawingViews& views = DrawingViews::Of(d);
    if (const dw::ViewLines* l = views.Lines(app.workspace, d, v)) {
        const Box2 b = dw::ViewBox(v, *l);
        if (!b.IsEmpty()) return b;
    }
    Box2 b;
    if (const auto bounds = views.ModelBounds(app.workspace, d, v.model)) {
        const Box2 e = dw::Extent(v.direction, BoxOf(*bounds));
        b.Include(v.at + e.min * v.scale.Value());
        b.Include(v.at + e.max * v.scale.Value());
    }
    if (b.IsEmpty()) b.Include(v.at, 15);
    return b;
}

/// A path of a model as the drawing keeps it: relative to a saved drawing, else full.
std::string ModelPath(const DrawingState& d, const std::filesystem::path& file) {
    if (d.savedAt.empty()) return DrawingState::StoredPath(file);
    std::filesystem::path relative = file.lexically_relative(d.savedAt.parent_path());
    if (relative.empty()) relative = file;
    return DrawingState::StoredPath(relative);
}

/// The model the views are made of: the drawing's first, else one the user picks now.
ObjectId ModelForViews(AppContext& app, DrawingState& d) {
    if (!d.drawing.models.empty()) return d.drawing.models.front().id;
    if (const std::string problem = platform::FileDialogProblem(); !problem.empty()) {
        app.notes.Error(problem);
        return {};
    }
    const std::filesystem::path folder = d.savedAt.empty() ? platform::DocumentsDir() : d.savedAt.parent_path();
    const auto file = platform::ShowOpenDialog(Tr("Деталь для чертежа"), {{Tr("Деталь BreezeCAD"), "*.bzc"}}, folder);
    if (!file) return {};
    ObjectId id;
    const std::string path = ModelPath(d, *file);
    d.Change(Tr("Модель чертежа"), [&](dw::Drawing& dr) {
        dw::ModelRef ref;
        ref.id   = dr.Ids().Next();
        ref.path = path;
        id       = ref.id;
        dr.models.push_back(std::move(ref));
    });
    return id;
}

/// The boxes of the views on the sheet now (to keep new ones off them).
std::vector<Box2> ViewBoxes(AppContext& app, DrawingState& d) {
    std::vector<Box2> out;
    for (const dw::View& v : dw::ViewsOf(d.Sheet())) {
        Box2 b = BoxOfView(app, d, v);
        b.Include(b.min - Vec2{5, 5});
        b.Include(b.max + Vec2{5, 5});
        out.push_back(b);
    }
    return out;
}

void AddStandardViews(AppContext& app) {
    DrawingState* d = ActiveDrawing(app);
    if (!d) return;
    const ObjectId model = ModelForViews(app, *d);
    if (!model) return;
    std::string problem;
    const auto bounds = DrawingViews::Of(*d).ModelBounds(app.workspace, *d, model, &problem);
    if (!bounds) {
        app.notes.Error(problem);
        return;
    }
    const dw::SheetLayout layout = dw::LayoutSheet(d->Sheet(), d->SheetNumber(), static_cast<int>(d->drawing.sheets.size()));
    const dw::Room room = dw::RoomOf(layout, ViewBoxes(app, *d));
    const auto place = dw::PlaceStandardViews(room, BoxOf(*bounds));
    if (!place) {
        app.notes.Info(Tr("Виды не помещаются на лист: возьмите формат больше или освободите место"));
        return;
    }
    const ObjectId sheet = d->Sheet().id;
    ObjectId main;
    d->Change(Tr("Стандартные виды"), [&](dw::Drawing& dr) {
        for (dw::Sheet& s : dr.sheets) {
            if (s.id != sheet) continue;
            dw::View front;
            front.id        = dr.Ids().Next();
            front.model     = model;
            front.direction = dw::ViewDirection::Front;
            front.scale     = place->scale;
            front.at        = place->front;
            dw::View top = front, left = front;
            top.id         = dr.Ids().Next();
            top.direction  = dw::ViewDirection::Top;
            top.at         = place->top;
            top.parent     = front.id;
            left.id        = dr.Ids().Next();
            left.direction = dw::ViewDirection::Left;
            left.at        = place->left;
            left.parent    = front.id;
            for (const dw::View& v : {front, top, left}) s.views.push_back(dw::MakeViewItem(v));
            // The scale of the title block: of the main view, unless the user wrote one.
            if (s.title.fields[dw::field::kScale].empty()) s.title.fields[dw::field::kScale] = dw::ScaleText(place->scale);
            main = front.id;
        }
    });
    d->selectedView = main;
}

void AddIsoView(AppContext& app) {
    DrawingState* d = ActiveDrawing(app);
    if (!d) return;
    const ObjectId model = ModelForViews(app, *d);
    if (!model) return;
    std::string problem;
    const auto bounds = DrawingViews::Of(*d).ModelBounds(app.workspace, *d, model, &problem);
    if (!bounds) {
        app.notes.Error(problem);
        return;
    }
    const dw::SheetLayout layout = dw::LayoutSheet(d->Sheet(), d->SheetNumber(), static_cast<int>(d->drawing.sheets.size()));
    const dw::Room room = dw::RoomOf(layout, ViewBoxes(app, *d));
    const Box2 e = dw::Extent(dw::ViewDirection::Iso, BoxOf(*bounds));
    // The scale of the views already there, or the largest that fits; smaller until a place is free.
    double limit = 1e9;
    for (const dw::View& v : dw::ViewsOf(d->Sheet())) limit = std::min(limit, v.scale.Value());
    for (const dw::Scale& sc : dw::StandardScales()) {
        if (sc.Value() > limit * (1 + 1e-9)) continue;
        const double s = sc.Value();
        const auto spot = dw::FreeSpot(room, (e.max - e.min) * s);
        if (!spot) continue;
        const ObjectId sheet = d->Sheet().id;
        ObjectId added;
        d->Change(Tr("Изометрия"), [&](dw::Drawing& dr) {
            for (dw::Sheet& sh : dr.sheets) {
                if (sh.id != sheet) continue;
                dw::View v;
                v.id        = dr.Ids().Next();
                v.model     = model;
                v.direction = dw::ViewDirection::Iso;
                v.scale     = sc;
                v.at        = *spot - e.min * s;
                v.hidden    = false;   // ГОСТ 2.305: an isometry without hidden lines
                v.axes      = false;
                sh.views.push_back(dw::MakeViewItem(v));
                added = v.id;
            }
        });
        d->selectedView = added;
        return;
    }
    app.notes.Info(Tr("Изометрия не помещается на лист: возьмите формат больше или освободите место"));
}

/// Changes one view of the sheet shown (an undo step).
void ChangeView(DrawingState& d, ObjectId id, const std::string& label, const std::function<void(dw::View&)>& change) {
    const ObjectId sheet = d.Sheet().id;
    d.Change(label, [&](dw::Drawing& dr) {
        for (dw::Sheet& s : dr.sheets)
            if (s.id == sheet)
                if (dw::Item* item = dw::FindViewItem(s, id))
                    if (auto v = dw::ReadView(*item)) {
                        change(*v);
                        dw::WriteView(*item, *v);
                    }
    });
}

void DeleteView(DrawingState& d, ObjectId id) {
    const ObjectId sheet = d.Sheet().id;
    d.Change(Tr("Удалить вид"), [&](dw::Drawing& dr) {
        for (dw::Sheet& s : dr.sheets) {
            if (s.id != sheet) continue;
            std::erase_if(s.views, [&](const dw::Item& i) {
                const auto v = dw::ReadView(i);
                return v && v->id == id;
            });
            // Views in projection with it stay, on their own.
            for (dw::Item& item : s.views)
                if (auto v = dw::ReadView(item); v && v->parent == id) {
                    v->parent = {};
                    dw::WriteView(item, *v);
                }
        }
    });
    if (d.selectedView == id) d.selectedView = {};
}

/// The menu of a view (right click on it or on its row in the tree).
void ViewMenu(DrawingState& d) {
    if (!ImGui::BeginPopup("##view-menu")) return;
    const auto views = dw::ViewsOf(d.Sheet());
    const auto it = std::find_if(views.begin(), views.end(), [&](const dw::View& v) { return v.id == d.selectedView; });
    if (it == views.end()) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    const dw::View v = *it;
    ImGui::TextDisabled("%s", ViewTitle(v).c_str());
    ImGui::Separator();
    if (ImGui::MenuItem(Tr("Невидимые линии"), nullptr, v.hidden))
        ChangeView(d, v.id, Tr("Невидимые линии"), [](dw::View& x) { x.hidden = !x.hidden; });
    if (ImGui::MenuItem(Tr("Осевые линии"), nullptr, v.axes))
        ChangeView(d, v.id, Tr("Осевые линии"), [](dw::View& x) { x.axes = !x.axes; });
    // The scale goes to the whole group in projection: the main view and the ones with it.
    const ObjectId main = v.parent ? v.parent : v.id;
    if (ImGui::BeginMenu(Tr("Масштаб"))) {
        for (const dw::Scale& sc : dw::StandardScales()) {
            if (sc.Value() > 20 || sc.Value() < 0.01) continue;   // the usual ones in the menu
            if (!ImGui::MenuItem(dw::ScaleText(sc).c_str(), nullptr, sc == v.scale)) continue;
            const ObjectId sheet = d.Sheet().id;
            d.Change(Tr("Масштаб вида"), [&](dw::Drawing& dr) {
                for (dw::Sheet& s : dr.sheets) {
                    if (s.id != sheet) continue;
                    std::optional<dw::View> head;
                    if (dw::Item* item = dw::FindViewItem(s, main)) head = dw::ReadView(*item);
                    for (dw::Item& item : s.views) {
                        auto x = dw::ReadView(item);
                        if (!x || (x->id != main && x->parent != main)) continue;
                        // The ones in projection keep their place from the main view, scaled.
                        if (head && x->id != main) x->at = head->at + (x->at - head->at) * (sc.Value() / x->scale.Value());
                        x->scale = sc;
                        dw::WriteView(item, *x);
                    }
                }
            });
        }
        ImGui::EndMenu();
    }
    ImGui::Separator();
    if (ImGui::MenuItem(Tr("Удалить вид"), "Delete")) DeleteView(d, v.id);
    ImGui::EndPopup();
}

/// Dragging a view: it and the views in projection with it.
struct ViewDrag {
    ObjectId view;
    Vec2     start;   ///< the sheet point grabbed
    bool     lockX = false, lockY = false;   ///< a view in projection moves along its link only
    std::vector<std::pair<ObjectId, Vec2>> from;   ///< the views moved and where they were
    bool     moved = false;
};

ViewDrag& Drag() {
    static ViewDrag drag;   // one sheet under the mouse at a time
    return drag;
}

/// The positions of the dragged views, set without an undo step (one comes on release).
void PlaceDragged(DrawingState& d, const ViewDrag& drag, Vec2 delta) {
    dw::Sheet& s = d.Sheet();
    for (const auto& [id, at] : drag.from)
        if (dw::Item* item = dw::FindViewItem(s, id))
            if (auto v = dw::ReadView(*item)) {
                v->at = at + delta;
                dw::WriteView(*item, *v);
            }
}

// --- the tree -------------------------------------------------------------------

bool TreeRow(ui::Ui& ui, const char* id, const char* icon, const std::string& text, float x, float y, float width, bool selected,
             bool muted, bool* doubleClicked = nullptr) {
    const ui::ThemeColors& c = ui.C();
    const float h = ui.S(28);
    ImGui::SetCursorScreenPos({x, y});
    const bool clicked = ImGui::InvisibleButton(id, {width, h});
    const bool hovered = ImGui::IsItemHovered();
    if (doubleClicked) *doubleClicked = hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (selected || hovered) dl->AddRectFilled({x, y}, {x + width, y + h}, ui::ToU32(selected ? c.selectedRow : c.hoverRow), ui.S(6));
    if (icon) ui.icons.Draw(dl, icon, {x + ui.S(8), y + ui.S(6)}, ui.S(16), muted ? c.textMuted : c.textSecondary);
    dl->AddText(ui.fonts.body, ui.S(13), {x + ui.S(icon ? 32 : 8), y + ui.S(6)}, ui::ToU32(muted ? c.textMuted : c.text), text.c_str());
    return clicked;
}

} // namespace

// --- new drawings -----------------------------------------------------------------

void NewDrawing(AppContext& app) {
    EndEditing(app);
    app.workspace.NewDrawing();
    app.shell.ribbonTab = RibbonTab::Model;
}

bool CanDrawingFromModel(AppContext& app) {
    const Session* s = app.Doc();
    if (!s || app.workspace.ShowsHome() || s->drawing) return false;
    if (s->host) s = s->host;   // a part edited in its assembly: the assembly's drawing
    return !s->path.empty();
}

void NewDrawingFromModel(AppContext& app) {
    if (!CanDrawingFromModel(app)) return;
    Session* model = app.Doc();
    if (model->host) model = model->host;
    std::string name, designation, material;
    if (model->assembly) {
        name        = model->assembly->assembly.info.name;
        designation = model->assembly->assembly.info.designation;
    } else {
        name        = model->doc.info.name;
        designation = model->doc.info.designation;
        material    = model->doc.info.material;
    }
    if (name.empty()) name = model->Title();
    const std::filesystem::path file = model->path;
    EndEditing(app);
    Session& s = app.workspace.NewDrawing();
    // The model by its full path until the drawing is saved; then relative to it.
    dw::ModelRef ref;
    ref.id   = s.drawing->drawing.Ids().Next();
    ref.path = DrawingState::StoredPath(file);
    s.drawing->drawing.models.push_back(std::move(ref));
    dw::TitleBlock& title = s.drawing->Sheet().title;
    title.fields[dw::field::kName] = name;
    if (!designation.empty()) title.fields[dw::field::kDesignation] = designation;
    if (!material.empty()) title.fields[dw::field::kMaterial] = material;
    s.untitledName = app.workspace.NextUntitledName(std::format("{} — чертёж ", model->Title()));
    app.shell.ribbonTab = RibbonTab::Model;
}

// --- commands -----------------------------------------------------------------------

void RegisterDrawingActions(AppContext& app) {
    ActionRegistry& a = app.actions;
    const auto inDrawing = [&app] { return ActiveDrawing(app) != nullptr; };
    a.Add({"file.newDrawing", N_("Новый чертёж"), "doc-drawing", 0, N_("Пустой лист А3 с рамкой и основной надписью по ЕСКД"), {},
           [&app] { NewDrawing(app); }, 0, N_("чертёж лист ескд")});
    a.Add({"drawing.fromModel", N_("Создать чертёж"), "doc-drawing", 0,
           N_("Чертёж открытой детали или сборки: основная надпись — из её свойств. Документ должен быть сохранён"),
           [&app] { return CanDrawingFromModel(app); }, [&app] { NewDrawingFromModel(app); }, 0, N_("чертёж лист ескд")});
    a.Add({"drawing.sheet", N_("Лист и надпись"), "doc-drawing", 0,
           N_("Формат и ориентация листа, поля основной надписи (двойной щелчок по надписи)"), inDrawing,
           [&app] { OpenSheetDialog(app); }, 0, N_("формат основная надпись штамп")});
    a.Add({"drawing.addSheet", N_("Добавить лист"), "ui-plus", 0, N_("Новый лист того же формата после текущего"), inDrawing, [&app] {
               DrawingState* d = ActiveDrawing(app);
               d->Change(Tr("Добавить лист"), [d](dw::Drawing& dr) {
                   dw::Sheet sheet;
                   sheet.id          = dr.Ids().Next();
                   sheet.name        = std::format("Лист {}", dr.sheets.size() + 1);
                   sheet.size        = d->Sheet().size;
                   sheet.orientation = d->Sheet().orientation;
                   const auto at = std::find_if(dr.sheets.begin(), dr.sheets.end(), [&](const dw::Sheet& s) { return s.id == d->Sheet().id; });
                   const ObjectId id = sheet.id;
                   dr.sheets.insert(at == dr.sheets.end() ? at : at + 1, std::move(sheet));
                   d->activeSheet = id;
               });
               d->zoom = 0;
           }});
    a.Add({"drawing.standardViews", N_("Стандартные виды"), "ds-hidden-lines", 0,
           N_("Главный вид, вид сверху и вид слева по детали чертежа (ГОСТ 2.305), в масштабе, который помещается на лист"), inDrawing,
           [&app] { AddStandardViews(app); }, 0, N_("вид спереди сверху слева проекции")});
    a.Add({"drawing.isoView", N_("Изометрия"), "ui-cube", 0, N_("Изометрия детали (ГОСТ 2.317) на свободном месте листа"), inDrawing,
           [&app] { AddIsoView(app); }, 0, N_("аксонометрия вид")});
    a.Add({"file.exportPdf", N_("Экспорт в PDF"), "export", 0, N_("Все листы чертежа — страницами в натуральную величину, для печати и отправки"),
           inDrawing, [&app] { ExportDocument(app, ExportFormat::Pdf); }, 0, N_("пдф печать вывод")});
    a.Add({"drawing.removeSheet", N_("Удалить лист"), "ui-trash", 0, N_("Убрать текущий лист из чертежа (Ctrl+Z вернёт)"),
           [&app] { DrawingState* d = ActiveDrawing(app); return d && d->drawing.sheets.size() > 1; }, [&app] {
               DrawingState* d = ActiveDrawing(app);
               d->Change(Tr("Удалить лист"), [d](dw::Drawing& dr) {
                   const ObjectId gone = d->Sheet().id;
                   const auto at = std::find_if(dr.sheets.begin(), dr.sheets.end(), [&](const dw::Sheet& s) { return s.id == gone; });
                   const std::size_t index = static_cast<std::size_t>(at - dr.sheets.begin());
                   dr.sheets.erase(at);
                   d->activeSheet = dr.sheets[std::min(index, dr.sheets.size() - 1)].id;
               });
               d->zoom = 0;
           }});
}

void FillDrawingRibbon(AppContext& app, RibbonSink& sink) {
    switch (app.shell.ribbonTab) {
        case RibbonTab::Model:   // «Виды»
            sink.BeginGroup(N_("Ассоциативные виды"));
            sink.Large("drawing.standardViews", N_("Стандартные\nвиды"));
            sink.LargeLater("ui-frame", N_("Проекционный\nвид"));
            sink.LargeLater("ui-arrow-left", N_("Вид по\nстрелке"));
            sink.Large("drawing.isoView", N_("Изометрия"));
            sink.EndGroup();
            sink.BeginGroup(N_("Разрезы"));
            sink.LargeLater("section", N_("Разрез /\nсечение"));
            sink.LargeLater("sk-circle", N_("Выносной\nэлемент"));
            sink.LargeLater("sk-rectangle", N_("Местный\nвид"));
            sink.EndGroup();
            break;
        case RibbonTab::Sketch:   // «Обозначения»
            sink.BeginGroup(N_("Размеры"));
            sink.LargeLater("dim-arrange", N_("Авто-\nразмер"));
            sink.LargeLater("sk-dimension", N_("Линейный"));
            sink.LargeLater("sk-circle", N_("Диамет-\nральный"));
            sink.LargeLater("ui-dimension", N_("Радиальный"));
            sink.LargeLater("a-angle", N_("Угловой"));
            sink.EndGroup();
            sink.BeginGroup(N_("Обозначения"));
            sink.LargeLater("a-axis", N_("Обозначение\nцентра"));
            sink.LargeLater("sk-centerline", N_("Осевая\nлиния"));
            sink.EndGroup();
            break;
        case RibbonTab::View:   // «Оформление»
            sink.BeginGroup(N_("Лист"));
            sink.Large("drawing.sheet", N_("Лист и\nнадпись"));
            sink.Large("drawing.addSheet", N_("Добавить\nлист"));
            sink.Large("drawing.removeSheet", N_("Удалить\nлист"));
            sink.EndGroup();
            sink.BeginGroup(N_("Вывод"));
            sink.Large("file.exportPdf", N_("Экспорт\nв PDF"));
            sink.EndGroup();
            break;
    }
}

// --- the tree -------------------------------------------------------------------------

void DrawDrawingTree(AppContext& app, Session& session, ImVec2 pos, ImVec2 size) {
    ui::Ui& ui = app.ui;
    const ui::ThemeColors& c = ui.C();
    DrawingState& d = *session.drawing;
    ImDrawList* dl = BeginArea("##drawing-tree", pos, size, c.panel, true);
    dl->AddLine({pos.x + size.x - 1, pos.y}, {pos.x + size.x - 1, pos.y + size.y}, ui::ToU32(c.divider));
    const float x = pos.x + ui.S(8), width = size.x - ui.S(17);
    float y = pos.y + ui.S(12);
    dl->AddText(ui.fonts.strong, ui.S(13), {x + ui.S(6), y}, ui::ToU32(c.text), Tr("Дерево чертежа"));
    y += ui.S(28);
    dl->AddLine({pos.x, y}, {pos.x + size.x - 1, y}, ui::ToU32(c.dividerSoft));
    y += ui.S(8);

    for (const dw::Sheet& sheet : d.drawing.sheets) {
        ImGui::PushID(static_cast<int>(sheet.id.value));
        const bool active = sheet.id == d.Sheet().id;
        bool twice = false;
        if (TreeRow(ui, "##sheet", "doc-drawing", SheetTitle(sheet), x, y, width, active, false, &twice) && !active) {
            d.activeSheet = sheet.id;
            d.zoom = 0;
        }
        if (twice) app.Later([&app] { OpenSheetDialog(app); });
        y += ui.S(30);
        if (active) {
            // Its views; what this version does not know is counted, kept as it is.
            const std::vector<dw::View> views = dw::ViewsOf(sheet);
            for (const dw::View& v : views) {
                ImGui::PushID(static_cast<int>(v.id.value));
                const bool picked = d.selectedView == v.id;
                const float indent = ui.S(v.parent ? 40 : 24);
                if (TreeRow(ui, "##view", v.direction == dw::ViewDirection::Iso ? "ui-cube" : "ui-frame", ViewTitle(v), x + indent, y,
                            width - indent, picked, false))
                    d.selectedView = v.id;
                if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                    d.selectedView = v.id;
                    ImGui::OpenPopup("##view-menu");
                }
                y += ui.S(30);
                ImGui::PopID();
            }
            const std::size_t newer = sheet.views.size() - views.size() + sheet.annotations.size();
            if (newer > 0) {
                TreeRow(ui, "##newer", nullptr, Trf("Объектов из более новой версии: {}", newer), x + ui.S(24), y, width - ui.S(24), false, true);
                y += ui.S(30);
            } else if (views.empty()) {
                TreeRow(ui, "##views", nullptr, Tr("Видов пока нет"), x + ui.S(24), y, width - ui.S(24), false, true);
                y += ui.S(30);
            }
        }
        ImGui::PopID();
    }

    y += ui.S(10);
    dl->AddText(ui.fonts.caption, ui.S(11), {x + ui.S(6), y}, ui::ToU32(c.textMuted), Tr("Модели чертежа"));
    y += ui.S(20);
    if (d.drawing.models.empty()) {
        ImGui::SetCursorScreenPos({x + ui.S(6), y});
        ImGui::PushTextWrapPos(x + width);
        ImGui::PushStyleColor(ImGuiCol_Text, ui::ToVec4(c.textMuted));
        ImGui::TextUnformatted(Tr("Нет. «Стандартные виды» на вкладке «Виды» предложат выбрать деталь."));
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
    }
    for (const dw::ModelRef& m : d.drawing.models) {
        ImGui::PushID(static_cast<int>(m.id.value));
        const std::string name = ToUtf8(PathFromUtf8(m.path).filename());
        TreeRow(ui, "##model", PathFromUtf8(m.path).extension() == ".bza" ? "assembly" : "tree-part", name, x, y, width, false, false);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", m.path.c_str());
        y += ui.S(30);
        ImGui::PopID();
    }
    ViewMenu(d);
    ImGui::SetCursorScreenPos({pos.x, y});
    ImGui::Dummy({1, ui.S(8)});
    EndArea();
}

// --- the sheet ------------------------------------------------------------------------

std::string DrawingSheetCaption(const DrawingState& d) {
    return Trf("{} из {}, {}", d.Sheet().name, d.drawing.sheets.size(), SizeName(d.Sheet().size));
}

void FitDrawingSheet(Session& session) {
    if (session.drawing) session.drawing->zoom = 0;
}

void DrawDrawingSheet(AppContext& app, Session& session, ImVec2 pos, ImVec2 size) {
    ui::Ui& ui = app.ui;
    const ui::ThemeColors& c = ui.C();
    DrawingState& d = *session.drawing;
    DrawingViews& views = DrawingViews::Of(d);
    views.Poll();
    ImDrawList* dl = BeginArea("##drawing-sheet", pos, size, c.sheetDesk);
    const dw::Sheet& sheet = d.Sheet();
    const dw::SheetLayout layout =
        dw::LayoutSheetWithViews(sheet, d.SheetNumber(), static_cast<int>(d.drawing.sheets.size()), LinesSource(app, d));
    if (d.zoom <= 0) FitInto(d, layout, size, ui.S(24), ui.S(36));

    // The canvas takes the mouse: wheel — zoom about the cursor; a view dragged
    // with the left button — moved; the sheet dragged with any button — moved.
    ImGui::SetCursorScreenPos(pos);
    ImGui::InvisibleButton("##sheet-canvas", size,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    SheetView view{{pos.x + size.x / 2, pos.y + size.y / 2}, d.center, d.zoom, layout.height};
    const ImGuiIO& io = ImGui::GetIO();
    if (hovered && io.MouseWheel != 0) {
        const Vec2 under = view.ToSheet(io.MousePos);
        const double fit = std::min(size.x / layout.width, size.y / layout.height);
        d.zoom = std::clamp(d.zoom * std::pow(1.15, io.MouseWheel), fit * 0.2, 60.0);
        view.zoom = d.zoom;
        const Vec2 now = view.ToSheet(io.MousePos);   // the point under the cursor stays there
        d.center = d.center + (under - now);
        view.center = d.center;
    }

    // The view under the cursor: the last drawn on top.
    std::vector<dw::View> onSheet = dw::ViewsOf(sheet);
    std::vector<Box2> boxes;
    for (const dw::View& v : onSheet) boxes.push_back(BoxOfView(app, d, v));
    const auto viewAt = [&](Vec2 p) -> ObjectId {
        for (std::size_t i = onSheet.size(); i-- > 0;) {
            const Box2& b = boxes[i];
            if (p.x >= b.min.x - 2 && p.x <= b.max.x + 2 && p.y >= b.min.y - 2 && p.y <= b.max.y + 2) return onSheet[i].id;
        }
        return {};
    };
    ViewDrag& drag = Drag();
    if (hovered && (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right))) {
        const Vec2 at = view.ToSheet(io.MousePos);
        d.selectedView = viewAt(at);
        drag = {};
        if (d.selectedView && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            const auto v = std::find_if(onSheet.begin(), onSheet.end(), [&](const dw::View& x) { return x.id == d.selectedView; });
            drag.view  = v->id;
            drag.start = at;
            // A view in projection moves along its link with the main one only (ГОСТ 2.305).
            if (v->parent) {
                drag.lockX = v->direction == dw::ViewDirection::Top || v->direction == dw::ViewDirection::Bottom;
                drag.lockY = !drag.lockX;
            }
            for (const dw::View& x : onSheet)
                if (x.id == v->id || (!v->parent && x.parent == v->id)) drag.from.push_back({x.id, x.at});
        }
    }
    if (drag.view && ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        Vec2 delta = view.ToSheet(io.MousePos) - drag.start;
        if (drag.lockX) delta.x = 0;
        if (drag.lockY) delta.y = 0;
        if (std::abs(delta.x) + std::abs(delta.y) > 0) drag.moved = true;
        PlaceDragged(d, drag, delta);
    } else if (ImGui::IsItemActive() && (io.MouseDelta.x != 0 || io.MouseDelta.y != 0)) {
        d.center = d.center + Vec2{-io.MouseDelta.x / d.zoom, io.MouseDelta.y / d.zoom};
        view.center = d.center;
    }
    if (drag.view && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        if (drag.moved) {
            // One undo step for the whole move: back where they were, then the change.
            std::vector<std::pair<ObjectId, Vec2>> to;
            for (const auto& [id, at] : drag.from)
                for (const dw::View& v : dw::ViewsOf(d.Sheet()))
                    if (v.id == id) to.push_back({id, v.at});
            PlaceDragged(d, drag, {0, 0});
            const ObjectId sheetId = d.Sheet().id;
            d.Change(Tr("Сдвинуть вид"), [&](dw::Drawing& dr) {
                for (dw::Sheet& s : dr.sheets)
                    if (s.id == sheetId)
                        for (const auto& [id, at] : to)
                            if (dw::Item* item = dw::FindViewItem(s, id))
                                if (auto v = dw::ReadView(*item)) {
                                    v->at = at;
                                    dw::WriteView(*item, *v);
                                }
            });
        }
        drag = {};
    }
    // Right click on a view (not a drag of the sheet): its menu.
    if (hovered && d.selectedView && ImGui::IsMouseReleased(ImGuiMouseButton_Right) &&
        ImGui::GetMouseDragDelta(ImGuiMouseButton_Right).x == 0 && ImGui::GetMouseDragDelta(ImGuiMouseButton_Right).y == 0)
        ImGui::OpenPopup("##view-menu");
    if (d.selectedView && (hovered || ImGui::IsWindowFocused()) && !ImGui::IsAnyItemActive() && ImGui::IsKeyPressed(ImGuiKey_Delete))
        DeleteView(d, d.selectedView);
    // Double click on the title block: fill it in (as in КОМПАС).
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        const Vec2 at = view.ToSheet(io.MousePos);
        if (at.x >= layout.titleBlock.min.x && at.x <= layout.titleBlock.max.x && at.y >= layout.titleBlock.min.y &&
            at.y <= layout.titleBlock.max.y)
            app.Later([&app] { OpenSheetDialog(app); });
    }

    ImGui::PushClipRect(pos, {pos.x + size.x, pos.y + size.y}, true);
    // Drawn from the layout of this frame's start; a view being dragged is drawn where it is now.
    const dw::SheetLayout now = drag.moved ? dw::LayoutSheetWithViews(d.Sheet(), d.SheetNumber(), static_cast<int>(d.drawing.sheets.size()),
                                                                       LinesSource(app, d))
                                           : layout;
    DrawSheet(dl, ui, view, now);
    // The views: the picked one framed; one without lines says why.
    for (const dw::View& v : dw::ViewsOf(d.Sheet())) {
        const Box2 b = BoxOfView(app, d, v);
        if (v.id == d.selectedView) {
            const ImVec2 a = view.ToScreen({b.min.x - 2, b.max.y + 2}), z = view.ToScreen({b.max.x + 2, b.min.y - 2});
            dl->AddRect(a, z, ui::ToU32(c.accent), ui.S(2), 0, ui.S(1.5f));
        }
        if (!views.Lines(app.workspace, d, v)) {
            const std::string why = views.Problem(v.id);
            const ImVec2 at = view.ToScreen({(b.min.x + b.max.x) / 2, (b.min.y + b.max.y) / 2});
            const ImVec2 ts = ui.fonts.body->CalcTextSizeA(ui.S(12), FLT_MAX, 0, why.c_str());
            dl->AddText(ui.fonts.body, ui.S(12), {at.x - ts.x / 2, at.y - ts.y / 2}, ui::ToU32(c.textMuted), why.c_str());
        }
    }
    ImGui::PopClipRect();

    // A note at the top, as over the part's view: what to do now.
    const std::size_t newer = sheet.views.size() - onSheet.size() + sheet.annotations.size();
    std::string note;
    if (newer > 0) note = Trf("На листе есть объекты из более новой версии BreezeCAD ({}): они сохранятся без изменений", newer);
    else if (onSheet.empty()) note = Tr("«Стандартные виды» на вкладке «Виды» — главный вид, сверху и слева по детали. Двойной щелчок по надписи — заполнить её");
    else if (views.Busy()) note = Tr("Виды считаются…");
    if (!note.empty()) {
        const ImVec2 ns = ui.fonts.body->CalcTextSizeA(ui.S(12), FLT_MAX, 0, note.c_str());
        const ImVec2 n0{pos.x + (size.x - ns.x) / 2 - ui.S(14), pos.y + ui.S(12)};
        dl->AddRectFilled(n0, {n0.x + ns.x + ui.S(28), n0.y + ns.y + ui.S(14)}, ui::ToU32(c.floatingButton), ui.S(14));
        dl->AddText(ui.fonts.body, ui.S(12), {n0.x + ui.S(14), n0.y + ui.S(7)}, ui::ToU32(c.textSecondary), note.c_str());
    }

    ViewMenu(d);
    // The whole sheet again: a button in the corner, as «Домой» over the model.
    ImGui::SetCursorScreenPos({pos.x + size.x - ui.S(48), pos.y + ui.S(12)});
    if (ui::IconButton(ui, "##sheet-fit", "view-fit", 36, 20, Tr("Лист целиком (F)"))) d.zoom = 0;
    EndArea();
}

// --- the sheet's window -----------------------------------------------------------------

void DrawDrawingDialogs(AppContext& app) {
    SheetDialog& dlg = Dialog();
    if (!dlg.open) return;
    DrawingState* d = ActiveDrawing(app);
    if (!d) {
        dlg.open = false;
        return;
    }
    ui::Ui& ui = app.ui;
    const ui::ThemeColors& c = ui.C();
    if (!BeginDialog(ui, "##drawing-sheet-dialog", dlg.open, ui.S(520))) return;
    {
        ui::FontScope font(ui.fonts.title);
        ImGui::TextUnformatted(Trf("{} — лист и основная надпись", dlg.edit.name).c_str());
    }
    ImGui::Dummy({0, ui.S(4)});
    const float full = ImGui::GetContentRegionAvail().x;

    ImGui::TextUnformatted(Tr("Формат"));
    std::vector<std::string> names;
    for (dw::SheetSize s : kSizes) names.push_back(SizeName(s));
    std::vector<const char*> labels;
    for (const std::string& n : names) labels.push_back(n.c_str());
    int size = 1;
    for (std::size_t i = 0; i < kSizes.size(); ++i)
        if (kSizes[i] == dlg.edit.size) size = static_cast<int>(i);
    size = ui::Segmented(ui, "##size", labels, size, static_cast<int>(labels.size()), full);
    dlg.edit.size = kSizes[static_cast<std::size_t>(size)];

    ImGui::TextUnformatted(Tr("Ориентация"));
    const char* orientations[] = {Tr("Книжная"), Tr("Альбомная")};
    const bool a4 = dlg.edit.size == dw::SheetSize::A4;   // ГОСТ 2.104: А4 — только вертикально
    int orientation = a4 || dlg.edit.orientation == dw::Orientation::Portrait ? 0 : 1;
    orientation = ui::Segmented(ui, "##orientation", orientations, orientation, 2, full, a4 ? 0b10 : 0);
    if (!a4) dlg.edit.orientation = orientation == 0 ? dw::Orientation::Portrait : dw::Orientation::Landscape;

    ImGui::Dummy({0, ui.S(6)});
    ImGui::PushStyleColor(ImGuiCol_Text, ui::ToVec4(c.textSecondary));
    ImGui::TextUnformatted(Tr("Основная надпись (ГОСТ 2.104, форма 1)"));
    ImGui::PopStyleColor();
    struct Field {
        const char* key;
        const char* label;
    };
    const Field fields[] = {{dw::field::kName, N_("Наименование")},       {dw::field::kDesignation, N_("Обозначение")},
                            {dw::field::kMaterial, N_("Материал")},       {dw::field::kMass, N_("Масса")},
                            {dw::field::kScale, N_("Масштаб")},           {dw::field::kLetter, N_("Литера")},
                            {dw::field::kOrganization, N_("Организация")}, {dw::field::kDeveloper, N_("Разработал")},
                            {dw::field::kChecker, N_("Проверил")},        {dw::field::kTechControl, N_("Т. контроль")},
                            {dw::field::kNormControl, N_("Н. контроль")}, {dw::field::kApprover, N_("Утвердил")}};
    const float labelWidth = ui.S(130);
    for (const Field& f : fields) {
        ImGui::PushID(f.key);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(Tr(f.label));
        ImGui::SameLine(labelWidth);
        ImGui::SetNextItemWidth(full - labelWidth);
        if (dlg.justOpened && std::string_view(f.key) == dw::field::kName) ImGui::SetKeyboardFocusHere();
        std::string value;
        const auto it = dlg.edit.title.fields.find(f.key);
        if (it != dlg.edit.title.fields.end()) value = it->second;
        if (ImGui::InputText("##value", &value)) dlg.edit.title.fields[f.key] = value;
        ImGui::PopID();
    }
    dlg.justOpened = false;

    ImGui::Dummy({0, ui.S(8)});
    const float bw = (full - ui.S(8)) / 2;
    const bool ok = ui::PrimaryButton(ui, "##ok", Tr("ОК"), "Enter", {bw, ui.S(36)}) ||
                    (ImGui::IsKeyPressed(ImGuiKey_Enter) && !ImGui::IsAnyItemActive());
    ImGui::SameLine(0, ui.S(8));
    const bool cancel = ui::SecondaryButton(ui, "##cancel", Tr("Отмена"), "Esc", {bw, ui.S(36)});
    if (ok) {
        const ObjectId id = dlg.sheet;
        const dw::Sheet edited = dlg.edit;
        d->Change(Tr("Лист и основная надпись"), [id, edited](dw::Drawing& dr) {
            for (dw::Sheet& s : dr.sheets)
                if (s.id == id) {
                    s.size        = edited.size;
                    s.orientation = edited.orientation;
                    s.title       = edited.title;
                }
        });
        d->zoom = 0;   // another size: the whole sheet again
    }
    if (ok || cancel) {
        dlg.open = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

} // namespace bz::app
