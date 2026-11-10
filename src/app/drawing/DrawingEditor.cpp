#include "app/drawing/DrawingEditor.h"

#include "app/Actions.h"
#include "app/Operations.h"
#include "app/Session.h"
#include "app/Workspace.h"
#include "app/drawing/DrawingViewsUi.h"
#include "app/shell/Shell.h"
#include "core/I18n.h"
#include "core/PathText.h"
#include "drawing/ModelView.h"
#include "drawing/SheetLayout.h"
#include "ui/Ui.h"
#include "ui/Widgets.h"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
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
    for (const dw::Label& label : l.labels) DrawLabel(dl, ui.fonts.body, v, label, ink);
}

/// The whole sheet: `margin` around it, `top` more above (the note there).
void FitInto(DrawingState& d, const dw::SheetLayout& l, ImVec2 size, float margin, float top) {
    const double zx = (size.x - 2 * margin) / l.width, zy = (size.y - 2 * margin - top) / l.height;
    d.zoom   = std::max(0.05, std::min(zx, zy));
    // Shifted down by half the note's room: the sheet sits under it.
    d.center = {l.width / 2, l.height / 2 + top / 2 / d.zoom};
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
    RegisterDrawingViewActions(app);   // views of the model (DrawingViewsUi)
}

void FillDrawingRibbon(AppContext& app, RibbonSink& sink) {
    switch (app.shell.ribbonTab) {
        case RibbonTab::Model:   // «Виды»
            sink.BeginGroup(N_("Ассоциативные виды"));
            sink.Large("drawing.standardViews", N_("Стандартные\nвиды"));
            sink.LargeLater("ui-frame", N_("Проекционный\nвид"));
            sink.LargeLater("ui-arrow-left", N_("Вид по\nстрелке"));
            sink.LargeLater("ui-cube", N_("Изометрия"));
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
            // What is on it: the views of the model (DrawingViewsUi), then what a newer version put there.
            const std::vector<ViewRow> views = SheetViewRows(d);
            for (const ViewRow& row : views) {
                ImGui::PushID(static_cast<int>(row.id.value));
                if (TreeRow(ui, "##view", "ds-hidden-lines", row.title, x + ui.S(24), y, width - ui.S(24), row.selected, false))
                    SelectView(d, row.id);
                ImGui::PopID();
                y += ui.S(30);
            }
            const std::size_t newer = dw::UnknownItemCount(sheet);
            if (newer > 0 || views.empty()) {
                const std::string what = newer > 0 ? Trf("Объектов из более новой версии: {}", newer) : std::string(Tr("Видов пока нет"));
                TreeRow(ui, "##views", nullptr, what, x + ui.S(24), y, width - ui.S(24), false, true);
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
        ImGui::TextUnformatted(Tr("Нет. Откройте деталь или сборку и выберите «Создать чертёж» в меню «Файл»."));
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
    ImDrawList* dl = BeginArea("##drawing-sheet", pos, size, c.sheetDesk);
    const dw::Sheet& sheet = d.Sheet();
    const dw::SheetLayout layout = dw::LayoutSheet(sheet, d.SheetNumber(), static_cast<int>(d.drawing.sheets.size()));
    if (d.zoom <= 0) FitInto(d, layout, size, ui.S(24), ui.S(36));

    // The canvas takes the mouse: wheel — zoom about the cursor, any button dragged — move the sheet.
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
    const bool holdsView = SheetViewsInput(app, session, {view.middle, view.center, view.zoom}, hovered);   // a view dragged: not the sheet
    if (!holdsView && ImGui::IsItemActive() && (io.MouseDelta.x != 0 || io.MouseDelta.y != 0)) {
        d.center = d.center + Vec2{-io.MouseDelta.x / d.zoom, io.MouseDelta.y / d.zoom};
        view.center = d.center;
    }
    // Double click on the title block: fill it in (as in КОМПАС).
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        const Vec2 at = view.ToSheet(io.MousePos);
        if (at.x >= layout.titleBlock.min.x && at.x <= layout.titleBlock.max.x && at.y >= layout.titleBlock.min.y &&
            at.y <= layout.titleBlock.max.y)
            app.Later([&app] { OpenSheetDialog(app); });
    }

    ImGui::PushClipRect(pos, {pos.x + size.x, pos.y + size.y}, true);
    DrawSheet(dl, ui, view, layout);
    DrawSheetViews(app, session, dl, {view.middle, view.center, view.zoom});
    ImGui::PopClipRect();

    // A note at the top, as over the part's view: what this sheet can do now.
    const std::size_t newer = dw::UnknownItemCount(d.Sheet());
    const std::string note = newer > 0 ? Trf("На листе есть виды из более новой версии BreezeCAD ({}): они сохранятся без изменений", newer)
                                       : SheetViewsNote(d);
    const ImVec2 ns = ui.fonts.body->CalcTextSizeA(ui.S(12), FLT_MAX, 0, note.c_str());
    const ImVec2 n0{pos.x + (size.x - ns.x) / 2 - ui.S(14), pos.y + ui.S(12)};
    dl->AddRectFilled(n0, {n0.x + ns.x + ui.S(28), n0.y + ns.y + ui.S(14)}, ui::ToU32(c.floatingButton), ui.S(14));
    dl->AddText(ui.fonts.body, ui.S(12), {n0.x + ui.S(14), n0.y + ui.S(7)}, ui::ToU32(c.textSecondary), note.c_str());

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
