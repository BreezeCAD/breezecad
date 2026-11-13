#include "app/drawing/DrawingEditor.h"

#include "app/Actions.h"
#include "app/Operations.h"
#include "app/Session.h"
#include "app/Workspace.h"
#include "app/drawing/DrawingViews.h"
#include "app/shell/Shell.h"
#include "core/I18n.h"
#include "core/PathText.h"
#include "drawing/Dimension.h"
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
#include <set>
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

std::string SheetTitle(const dw::Sheet& s) {
    if (s.kind == dw::SheetKind::Specification) return s.name;
    return std::format("{} ({})", s.name, SizeName(s.size));
}

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

/// An arrow of a dimension.
void DrawFill(ImDrawList* dl, const SheetView& v, const dw::Fill& f, ImU32 color) {
    if (f.points.size() < 3) return;
    std::vector<ImVec2> points;
    for (Vec2 p : f.points) points.push_back(v.ToScreen(p));
    dl->AddConvexPolyFilled(points.data(), static_cast<int>(points.size()), color);
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
    const ImU32 lost = ui::ToU32(c.danger);   // a dimension whose edges are gone
    for (const dw::Polyline& p : l.polylines) DrawPolyline(dl, v, p, p.problem ? lost : ink, thick, thin);
    for (const dw::Fill& f : l.fills) DrawFill(dl, v, f, f.problem ? lost : ink);
    for (const dw::Label& label : l.labels) DrawLabel(dl, ui.fonts.body, v, label, label.problem ? lost : ink);
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
        case dw::ViewDirection::Section: return N_("Разрез");
    }
    return N_("Вид спереди");
}

std::string ViewTitle(const dw::View& v) {
    if (v.section) return Trf("Разрез {}–{} ({})", v.section->letter, v.section->letter, dw::ScaleText(v.scale));
    return std::format("{} ({})", Tr(DirectionName(v.direction)), dw::ScaleText(v.scale));
}

/// The index of the sheet shown in the drawing.
std::size_t SheetIndex(const DrawingState& d) {
    for (std::size_t i = 0; i < d.drawing.sheets.size(); ++i)
        if (d.drawing.sheets[i].id == d.Sheet().id) return i;
    return 0;
}

/// The page shown: the sheet, or the page of its specification picked.
dw::Page CurrentPage(DrawingState& d, const dw::SheetContent& content) {
    const std::vector<dw::Page> pages = dw::PagesOf(d.drawing, SheetIndex(d), content);
    if (pages.empty()) return {};
    d.activePage = std::clamp(d.activePage, 0, static_cast<int>(pages.size()) - 1);
    return pages[static_cast<std::size_t>(d.activePage)];
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
        const Box2 e = dw::Extent(v, BoxOf(*bounds));
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

/// «Спецификация»: a sheet of the drawing's assembly (one picked if it has none).
void AddSpecification(AppContext& app) {
    DrawingState* d = ActiveDrawing(app);
    if (!d) return;
    ObjectId model;
    for (const dw::ModelRef& m : d->drawing.models)
        if (PathFromUtf8(m.path).extension() == ".bza") {
            model = m.id;
            break;
        }
    if (!model) {
        if (const std::string problem = platform::FileDialogProblem(); !problem.empty()) {
            app.notes.Error(problem);
            return;
        }
        const std::filesystem::path folder = d->savedAt.empty() ? platform::DocumentsDir() : d->savedAt.parent_path();
        const auto file = platform::ShowOpenDialog(Tr("Сборка для спецификации"), {{Tr("Сборка BreezeCAD"), "*.bza"}}, folder);
        if (!file) return;
        const std::string path = ModelPath(*d, *file);
        d->Change(Tr("Модель чертежа"), [&](dw::Drawing& dr) {
            dw::ModelRef ref;
            ref.id   = dr.Ids().Next();
            ref.path = path;
            model    = ref.id;
            dr.models.push_back(std::move(ref));
        });
    }
    for (const dw::Sheet& s : d->drawing.sheets)
        if (s.kind == dw::SheetKind::Specification && s.model == model) {   // one is enough: show it
            d->activeSheet = s.id;
            d->activePage = 0;
            d->zoom = 0;
            return;
        }
    ObjectId added;
    // The signatures and the organization as on the drawing's first sheet: one team signs both.
    dw::Fields signed_;
    for (const dw::Sheet& s : d->drawing.sheets)
        if (s.kind == dw::SheetKind::Drawing) {
            for (const char* key : {dw::field::kDeveloper, dw::field::kChecker, dw::field::kNormControl, dw::field::kApprover,
                                    dw::field::kOrganization, dw::field::kLetter})
                if (const auto it = s.title.fields.find(key); it != s.title.fields.end()) signed_[key] = it->second;
            break;
        }
    d->Change(Tr("Спецификация"), [&](dw::Drawing& dr) {
        dw::Sheet sheet;
        for (const auto& [key, value] : signed_) sheet.title.fields[key] = value;
        sheet.id          = dr.Ids().Next();
        sheet.name        = "Спецификация";
        sheet.kind        = dw::SheetKind::Specification;
        sheet.model       = model;
        sheet.size        = dw::SheetSize::A4;
        sheet.orientation = dw::Orientation::Portrait;
        added             = sheet.id;
        dr.sheets.push_back(std::move(sheet));
    });
    d->activeSheet = added;
    d->activePage = 0;
    d->zoom = 0;
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

std::optional<std::pair<ObjectId, Vec2>> AnnotationPlace(const dw::Item& item);

void DeleteView(DrawingState& d, ObjectId id) {
    const ObjectId sheet = d.Sheet().id;
    d.Change(Tr("Удалить вид"), [&](dw::Drawing& dr) {
        for (dw::Sheet& s : dr.sheets) {
            if (s.id != sheet) continue;
            std::erase_if(s.views, [&](const dw::Item& i) {
                const auto v = dw::ReadView(i);
                return v && v->id == id;
            });
            // Its dimensions go with it.
            std::erase_if(s.annotations, [&](const dw::Item& i) {
                const auto place = AnnotationPlace(i);
                return place && place->first == id;
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

// --- dimensions (ADR-0012, step 5) ------------------------------------------------------

/// Changes one dimension of the sheet shown (an undo step).
void ChangeDimension(DrawingState& d, ObjectId id, const std::string& label, const std::function<void(dw::Dimension&)>& change) {
    const ObjectId sheet = d.Sheet().id;
    d.Change(label, [&](dw::Drawing& dr) {
        for (dw::Sheet& s : dr.sheets)
            if (s.id == sheet)
                if (dw::Item* item = dw::FindAnnotation(s, id))
                    if (auto dim = dw::ReadDimension(*item)) {
                        change(*dim);
                        dw::WriteDimension(*item, *dim);
                    }
    });
}

/// The view of a dimension or a position this version reads, and where it stands ("at"); nullopt for others.
std::optional<std::pair<ObjectId, Vec2>> AnnotationPlace(const dw::Item& item) {
    if (const auto dim = dw::ReadDimension(item)) return std::pair{dim->view, dim->at};
    if (const auto b = dw::ReadBalloon(item)) return std::pair{b->view, b->at};
    return std::nullopt;
}

void MoveAnnotation(dw::Item& item, Vec2 at) {
    if (auto dim = dw::ReadDimension(item)) {
        dim->at = at;
        dw::WriteDimension(item, *dim);
    } else if (auto b = dw::ReadBalloon(item)) {
        b->at = at;
        dw::WriteBalloon(item, *b);
    }
}

void DeleteAnnotation(DrawingState& d, ObjectId id) {
    const ObjectId sheet = d.Sheet().id;
    const bool balloon = std::any_of(d.Sheet().annotations.begin(), d.Sheet().annotations.end(), [&](const dw::Item& i) {
        const auto b = dw::ReadBalloon(i);
        return b && b->id == id;
    });
    d.Change(balloon ? Tr("Удалить позицию") : Tr("Удалить размер"), [&](dw::Drawing& dr) {
        for (dw::Sheet& s : dr.sheets)
            if (s.id == sheet)
                std::erase_if(s.annotations, [&](const dw::Item& i) {
                    const auto dim = dw::ReadDimension(i);
                    const auto b = dw::ReadBalloon(i);
                    return (dim && dim->id == id) || (b && b->id == id);
                });
    });
    if (d.selectedAnnotation == id) d.selectedAnnotation = {};
}

/// The menu of a position: delete it.
void BalloonMenu(DrawingState& d) {
    if (!ImGui::BeginPopup("##balloon-menu")) return;
    if (ImGui::MenuItem(Tr("Удалить позицию"), "Delete")) DeleteAnnotation(d, d.selectedAnnotation);
    ImGui::EndPopup();
}

/// The menu of a dimension: how a linear one measures, its text, delete.
void DimensionMenu(DrawingState& d) {
    if (!ImGui::BeginPopup("##dim-menu")) return;
    const auto dims = dw::DimensionsOf(d.Sheet());
    const auto it = std::find_if(dims.begin(), dims.end(), [&](const dw::Dimension& x) { return x.id == d.selectedAnnotation; });
    if (it == dims.end()) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    const dw::Dimension dim = *it;
    if (dim.kind == dw::DimensionKind::Linear) {
        const std::pair<dw::DimensionAxis, const char*> axes[] = {{dw::DimensionAxis::Horizontal, N_("Горизонтальный")},
                                                                  {dw::DimensionAxis::Vertical, N_("Вертикальный")},
                                                                  {dw::DimensionAxis::Aligned, N_("Параллельный")}};
        for (const auto& [axis, name] : axes)
            if (ImGui::MenuItem(Tr(name), nullptr, dim.axis == axis) && dim.axis != axis)
                ChangeDimension(d, dim.id, Tr("Вид размера"), [axis](dw::Dimension& x) { x.axis = axis; });
        ImGui::Separator();
    }
    static std::string text;
    if (ImGui::IsWindowAppearing()) text = dim.text.empty() ? "<>" : dim.text;
    ImGui::TextUnformatted(Tr("Текст размера («<>» — значение):"));
    ImGui::SetNextItemWidth(220);
    if (ImGui::InputText("##dim-text", &text, ImGuiInputTextFlags_EnterReturnsTrue)) {
        const std::string now = text == "<>" ? std::string{} : text;
        if (now != dim.text) ChangeDimension(d, dim.id, Tr("Текст размера"), [now](dw::Dimension& x) { x.text = now; });
        ImGui::CloseCurrentPopup();
    }
    ImGui::Separator();
    if (ImGui::MenuItem(Tr("Удалить размер"), "Delete")) DeleteAnnotation(d, dim.id);
    ImGui::EndPopup();
}

/// A section being made (ГОСТ 2.305): the cutting line's two ends on a view,
/// then the place of the section; the sight goes toward that place.
struct SectionTool {
    bool     active = false;
    ObjectId parent;
    std::vector<Vec2> points;   ///< the line's ends picked, in the parent's plane (1:1)
    void Reset() {
        parent = {};
        points.clear();
    }
};

SectionTool& Cutter() {
    static SectionTool tool;
    return tool;
}

/// The second end of the cutting line: across or along the sheet when it is
/// within 10° of it (hold Ctrl for any angle).
Vec2 SnapCut(Vec2 a, Vec2 b, bool free) {
    if (free) return b;
    const Vec2 d = b - a;
    const double angle = std::atan2(std::abs(d.y), std::abs(d.x)) * 180.0 / kPi;
    if (angle < 10) return {b.x, a.y};
    if (angle > 80) return {a.x, b.y};
    return b;
}

/// The section view the tool makes when put at `cursor` (sheet mm); nullopt without the model's box.
std::optional<dw::View> SectionAt(AppContext& app, DrawingState& d, const dw::View& parent, Vec2 a, Vec2 b, Vec2 cursor) {
    const auto bounds = DrawingViews::Of(d).ModelBounds(app.workspace, d, parent.model);
    if (!bounds || (b - a).Length() < 1e-6) return std::nullopt;
    dw::View v;
    v.model     = parent.model;
    v.direction = dw::ViewDirection::Section;
    v.scale     = parent.scale;
    v.hidden    = false;   // ГОСТ 2.305: a section shows no hidden contour as a rule
    v.section   = dw::MakeCut(parent, a, b, dw::ViewPointOf(parent, cursor), dw::NextSectionLetter(d.Sheet()));
    const Box2 e = dw::Extent(v, BoxOf(*bounds));
    v.at = cursor - (e.min + e.max) * 0.5 * v.scale.Value();
    return v;
}

/// A position being placed (ГОСТ 2.109): a click on a component of an
/// assembly view sets the dot, the next one the shelf.
struct BalloonTool {
    bool     active = false;
    ObjectId view, component;
    Vec3     point;    ///< the dot in the component's part coordinates
    bool     picked = false;
    void Reset() {
        view = component = {};
        picked = false;
    }
};

BalloonTool& Pointer() {
    static BalloonTool tool;
    return tool;
}

/// The component of an assembly view under a sheet point, and the point on
/// it (part coordinates): the edge picked there, its component the smallest
/// whose box holds it.
std::optional<std::pair<ObjectId, Vec3>> ComponentAt(AppContext& app, DrawingState& d, const dw::View& v, Vec2 at) {
    DrawingViews& views = DrawingViews::Of(d);
    const std::vector<DrawingViews::Part> parts = views.Parts(app.workspace, d, v.model);
    if (parts.empty()) return std::nullopt;
    dw::Json ref;
    const auto e = views.EdgeAt(app.workspace, d, v, at, 6.0 / std::max(d.zoom, 1e-6), ref);
    if (!e) return std::nullopt;
    const Vec3 p = e->kind == dw::ModelEdge::Kind::Circle ? e->center + (e->a - e->center) : (e->a + e->b) * 0.5;
    const DrawingViews::Part* best = nullptr;
    double volume = 1e300;
    for (const DrawingViews::Part& part : parts) {
        const Box3& b = part.box;
        constexpr double tol = 0.01;
        if (p.x < b.min.x - tol || p.y < b.min.y - tol || p.z < b.min.z - tol || p.x > b.max.x + tol || p.y > b.max.y + tol || p.z > b.max.z + tol)
            continue;
        const double vol = (b.max.x - b.min.x) * (b.max.y - b.min.y) * (b.max.z - b.min.z);
        if (vol < volume) {
            volume = vol;
            best = &part;
        }
    }
    if (!best) return std::nullopt;
    return std::pair{best->id, best->placement.Inverse().Point(p)};
}

/// «Расставить позиции»: one position per row of the specification that has
/// none on the sheet yet, the dot in the middle of the first such component,
/// the shelves in a column beside the view.
void ArrangeBalloons(AppContext& app) {
    DrawingState* d = ActiveDrawing(app);
    if (!d) return;
    const std::vector<dw::View> onSheet = dw::ViewsOf(d->Sheet());
    DrawingViews& views = DrawingViews::Of(*d);
    // The picked view, else the first one of an assembly.
    std::optional<dw::View> view;
    for (const dw::View& v : onSheet)
        if (v.id == d->selectedView && !views.Parts(app.workspace, *d, v.model).empty()) view = v;
    for (const dw::View& v : onSheet)
        if (!view && !v.section && v.direction != dw::ViewDirection::Iso && !views.Parts(app.workspace, *d, v.model).empty()) view = v;
    if (!view) {
        app.notes.Info(Tr("Позиции ставятся на виды сборки: на листе таких нет"));
        return;
    }
    const dw::ViewLines* lines = views.Lines(app.workspace, *d, *view);
    if (!lines) {
        app.notes.Info(Tr("Вид ещё считается — повторите через секунду"));
        return;
    }
    std::set<std::string> have;
    for (const dw::Balloon& b : dw::BalloonsOf(d->Sheet())) {
        if (const auto t = views.BalloonTarget(app.workspace, *d, *view, b)) have.insert(t->number);
    }
    std::vector<dw::Balloon> made;
    std::vector<Vec2> dots;
    for (const DrawingViews::Part& part : views.Parts(app.workspace, *d, view->model)) {
        dw::Balloon b;
        b.view = view->id;
        b.component = part.id;
        b.point = part.placement.Inverse().Point((part.box.min + part.box.max) * 0.5);
        const auto t = views.BalloonTarget(app.workspace, *d, *view, b);
        if (!t || have.count(t->number)) continue;
        have.insert(t->number);
        dots.push_back(view->at + dw::ProjectPoint(*view, t->anchor) * view->scale.Value());
        made.push_back(b);
    }
    if (made.empty()) {
        app.notes.Info(Tr("У всех позиций спецификации уже есть выноски"));
        return;
    }
    const Box2 box = dw::ViewBox(*view, *lines);
    const std::vector<Vec2> shelves = dw::ArrangeShelves(box, dots, dw::SheetSizeMm(d->Sheet().size, d->Sheet().orientation).x);
    const ObjectId sheetId = d->Sheet().id;
    d->Change(Tr("Расставить позиции"), [&](dw::Drawing& dr) {
        for (dw::Sheet& s : dr.sheets)
            if (s.id == sheetId)
                for (std::size_t i = 0; i < made.size(); ++i) {
                    dw::Balloon b = made[i];
                    b.id = dr.Ids().Next();
                    b.at = dw::ViewPointOf(*view, shelves[i]);
                    s.annotations.push_back(dw::MakeBalloonItem(b));
                }
    });
}

/// A dimension being made: its edges picked one by one on a view, then its
/// line placed with a click (as in КОМПАС: the command stays on for the next one).
struct DimensionTool {
    bool                          active = false;
    dw::DimensionKind             kind = dw::DimensionKind::Linear;
    ObjectId                      view;
    std::vector<dw::Json>         refs;
    std::vector<dw::ModelEdge>    edges;

    std::size_t Needs() const { return kind == dw::DimensionKind::Linear || kind == dw::DimensionKind::Angle ? 2 : 1; }
    /// Enough edges to show the dimension under the cursor.
    bool CanPlace() const { return !edges.empty() && (kind != dw::DimensionKind::Angle || edges.size() == 2); }
    void Reset() {
        view = {};
        refs.clear();
        edges.clear();
    }
};

DimensionTool& Tool() {
    static DimensionTool tool;   // one sheet edited at a time
    return tool;
}

void StartDimension(AppContext& app, dw::DimensionKind kind) {
    Cutter().active = false;
    Pointer().active = false;
    DimensionTool& t = Tool();
    t.Reset();
    t.active = true;
    t.kind = kind;
    if (DrawingState* d = ActiveDrawing(app)) {
        d->selectedAnnotation = {};
        if (dw::ViewsOf(d->Sheet()).empty()) {
            app.notes.Info(Tr("На листе нет видов: размеры ставятся на виды модели"));
            t.active = false;
        }
    }
}

void StartSectionTool(AppContext& app) {
    Tool().active = false;
    Pointer().active = false;
    SectionTool& t = Cutter();
    t.Reset();
    t.active = true;
    if (DrawingState* d = ActiveDrawing(app)) {
        if (d->selectedView) t.parent = d->selectedView;
        if (dw::ViewsOf(d->Sheet()).empty()) {
            app.notes.Info(Tr("На листе нет видов: разрез проводится по виду модели"));
            t.active = false;
        }
    }
}

/// The dimension the tool makes with its line through `cursor` (sheet mm).
/// A linear one measures across or along by where the cursor is, as in КОМПАС:
/// above or below its points — horizontal, beside them — vertical, else along them.
dw::Dimension ToolDimension(const DimensionTool& t, const dw::View& view, Vec2 cursor) {
    dw::Dimension dim;
    dim.view  = view.id;
    dim.kind  = t.kind;
    dim.edges = t.refs;
    dim.at    = dw::ViewPointOf(view, cursor);
    if (t.kind != dw::DimensionKind::Linear) return dim;
    std::vector<std::optional<dw::ModelEdge>> edges(t.edges.begin(), t.edges.end());
    dim.axis = dw::DimensionAxis::Aligned;
    const auto m = dw::MeasureOf(view, dim, edges);
    if (!m) return dim;
    const double x0 = std::min(m->p1.x, m->p2.x), x1 = std::max(m->p1.x, m->p2.x);
    const double y0 = std::min(m->p1.y, m->p2.y), y1 = std::max(m->p1.y, m->p2.y);
    const bool outY = cursor.y < y0 || cursor.y > y1, outX = cursor.x < x0 || cursor.x > x1;
    if (outY && !outX) dim.axis = dw::DimensionAxis::Horizontal;
    else if (outX && !outY) dim.axis = dw::DimensionAxis::Vertical;
    // A diameter of a shaft seen from its side (one circle, linear) measures across it.
    return dim;
}

const char* ToolPrompt(const DimensionTool& t) {
    switch (t.kind) {
        case dw::DimensionKind::Linear:
            return t.edges.empty() ? N_("Линейный размер: укажите ребро на виде (Esc — выйти)")
                   : t.edges.size() == 1 ? N_("Второе ребро — размер между ними; щелчок мимо рёбер — длина этого ребра")
                                         : N_("Щелчок — поставить размер. Выше или ниже — горизонтальный, сбоку — вертикальный");
        case dw::DimensionKind::Diameter:
            return t.edges.empty() ? N_("Диаметр: укажите окружность или дугу (Esc — выйти)") : N_("Щелчок — поставить размер");
        case dw::DimensionKind::Radius:
            return t.edges.empty() ? N_("Радиус: укажите дугу или окружность (Esc — выйти)") : N_("Щелчок — поставить размер");
        case dw::DimensionKind::Angle:
            return t.edges.empty()     ? N_("Угловой размер: укажите первое прямое ребро (Esc — выйти)")
                   : t.edges.size() == 1 ? N_("Укажите второе прямое ребро")
                                         : N_("Щелчок — поставить размер в нужном углу");
    }
    return "";
}

/// Dragging a dimension: its line and text move, its edges stay.
struct DimensionDrag {
    ObjectId id;
    Vec2     start;   ///< the sheet point grabbed
    Vec2     from;    ///< its `at` then
    double   scale = 1;
    bool     moved = false;
};

DimensionDrag& DimDrag() {
    static DimensionDrag drag;
    return drag;
}

/// An edge picked for a dimension, drawn over the view.
void DrawPickedEdge(ImDrawList* dl, const SheetView& sv, const dw::View& v, const dw::ModelEdge& e, ImU32 color, float width) {
    std::vector<ImVec2> points;
    if (e.kind == dw::ModelEdge::Kind::Circle) {
        Vec3 x = e.axis.Cross(Vec3{0, 0, 1});
        if (x.Length() < 1e-6) x = e.axis.Cross(Vec3{1, 0, 0});
        x = x.Normalized();
        const Vec3 y = e.axis.Normalized().Cross(x);
        for (int i = 0; i <= 64; ++i) {
            const double t = 2 * kPi * i / 64;
            points.push_back(sv.ToScreen(dw::SheetPointOf(v, e.center + (x * std::cos(t) + y * std::sin(t)) * e.radius)));
        }
    } else {
        points = {sv.ToScreen(dw::SheetPointOf(v, e.a)), sv.ToScreen(dw::SheetPointOf(v, e.b))};
    }
    dl->AddPolyline(points.data(), static_cast<int>(points.size()), color, ImDrawFlags_None, width);
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
             bool muted, bool* doubleClicked = nullptr, bool problem = false) {
    const ui::ThemeColors& c = ui.C();
    const float h = ui.S(28);
    ImGui::SetCursorScreenPos({x, y});
    const bool clicked = ImGui::InvisibleButton(id, {width, h});
    const bool hovered = ImGui::IsItemHovered();
    if (doubleClicked) *doubleClicked = hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (selected || hovered) dl->AddRectFilled({x, y}, {x + width, y + h}, ui::ToU32(selected ? c.selectedRow : c.hoverRow), ui.S(6));
    if (icon) ui.icons.Draw(dl, icon, {x + ui.S(8), y + ui.S(6)}, ui.S(16), muted ? c.textMuted : c.textSecondary);
    const Color& ink = problem ? c.danger : muted ? c.textMuted : c.text;
    dl->AddText(ui.fonts.body, ui.S(13), {x + ui.S(icon ? 32 : 8), y + ui.S(6)}, ui::ToU32(ink), text.c_str());
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
    const std::filesystem::path file = model->path;
    EndEditing(app);
    Session& s = app.workspace.NewDrawing();
    // The model by its full path until the drawing is saved; then relative to it.
    dw::ModelRef ref;
    ref.id   = s.drawing->drawing.Ids().Next();
    ref.path = DrawingState::StoredPath(file);
    s.drawing->drawing.models.push_back(std::move(ref));
    // The title block is not copied: its fields come from the model (DrawingViews::AutoFields) and follow it.
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
                   sheet.id   = dr.Ids().Next();
                   sheet.name = std::format("Лист {}", dr.sheets.size() + 1);
                   if (d->Sheet().kind == dw::SheetKind::Drawing) {   // the format of the one shown; after a specification, А3
                       sheet.size        = d->Sheet().size;
                       sheet.orientation = d->Sheet().orientation;
                   }
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
    a.Add({"drawing.section", N_("Разрез"), "section", 0,
           N_("Разрез по линии на виде (ГОСТ 2.305): два щелчка — концы линии, третий — место разреза; штриховка по ГОСТ 2.306"),
           [&app] {
               DrawingState* d = ActiveDrawing(app);
               return d && d->Sheet().kind == dw::SheetKind::Drawing;
           },
           [&app] { StartSectionTool(app); }, 0, N_("разрез сечение штриховка")});
    a.Add({"drawing.balloon", N_("Позиция"), "ui-marker", 0,
           N_("Позиционная выноска сборки (ГОСТ 2.109): щелчок по детали на виде, затем место полки; номер — из спецификации"),
           [&app] {
               DrawingState* d = ActiveDrawing(app);
               return d && d->Sheet().kind == dw::SheetKind::Drawing;
           },
           [&app] {
               Tool().active = false;
               Cutter().active = false;
               Pointer().Reset();
               Pointer().active = true;
           },
           0, N_("позиция выноска номер сборка")});
    a.Add({"drawing.arrangeBalloons", N_("Расставить позиции"), "ui-list", 0,
           N_("Выноски всех позиций спецификации, которых ещё нет на листе, — столбцом у вида сборки"),
           [&app] {
               DrawingState* d = ActiveDrawing(app);
               return d && d->Sheet().kind == dw::SheetKind::Drawing;
           },
           [&app] { ArrangeBalloons(app); }, 0, N_("позиции выноски расставить")});
    a.Add({"drawing.isoView", N_("Изометрия"), "ui-cube", 0, N_("Изометрия детали (ГОСТ 2.317) на свободном месте листа"), inDrawing,
           [&app] { AddIsoView(app); }, 0, N_("аксонометрия вид")});
    const auto onSheet = [&app] {
        DrawingState* d = ActiveDrawing(app);
        return d && d->Sheet().kind == dw::SheetKind::Drawing;
    };
    a.Add({"drawing.dimLinear", N_("Линейный размер"), "sk-dimension", 0,
           N_("Длина ребра или расстояние между двумя рёбрами вида; размер следует за моделью (ГОСТ 2.307)"), onSheet,
           [&app] { StartDimension(app, dw::DimensionKind::Linear); }, 0, N_("размер длина расстояние")});
    a.Add({"drawing.dimDiameter", N_("Диаметральный размер"), "sk-circle", 0, N_("Диаметр окружности или дуги вида — со знаком Ø"), onSheet,
           [&app] { StartDimension(app, dw::DimensionKind::Diameter); }, 0, N_("размер диаметр отверстие вал")});
    a.Add({"drawing.dimRadius", N_("Радиальный размер"), "ui-dimension", 0, N_("Радиус дуги или окружности вида — со знаком R"), onSheet,
           [&app] { StartDimension(app, dw::DimensionKind::Radius); }, 0, N_("размер радиус скругление")});
    a.Add({"drawing.dimAngle", N_("Угловой размер"), "a-angle", 0, N_("Угол между двумя прямыми рёбрами вида"), onSheet,
           [&app] { StartDimension(app, dw::DimensionKind::Angle); }, 0, N_("размер угол")});
    a.Add({"drawing.specification", N_("Спецификация"), "ui-list", 0,
           N_("Спецификация сборки по ГОСТ 2.106 на листах А4: разделы, позиции, количество — из сборки, меняется вместе с ней"),
           inDrawing, [&app] { AddSpecification(app); }, 0, N_("спецификация bom перечень состав сборки")});
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
            sink.Large("drawing.section", N_("Разрез /\nсечение"), Cutter().active);
            sink.LargeLater("sk-circle", N_("Выносной\nэлемент"));
            sink.LargeLater("sk-rectangle", N_("Местный\nвид"));
            sink.EndGroup();
            break;
        case RibbonTab::Sketch:   // «Обозначения»
            sink.BeginGroup(N_("Размеры"));
            sink.LargeLater("dim-arrange", N_("Авто-\nразмер"));
            sink.Large("drawing.dimLinear", N_("Линейный"), Tool().active && Tool().kind == dw::DimensionKind::Linear);
            sink.Large("drawing.dimDiameter", N_("Диамет-\nральный"), Tool().active && Tool().kind == dw::DimensionKind::Diameter);
            sink.Large("drawing.dimRadius", N_("Радиальный"), Tool().active && Tool().kind == dw::DimensionKind::Radius);
            sink.Large("drawing.dimAngle", N_("Угловой"), Tool().active && Tool().kind == dw::DimensionKind::Angle);
            sink.EndGroup();
            sink.BeginGroup(N_("Позиции"));
            sink.Large("drawing.balloon", N_("Позиция"), Pointer().active);
            sink.Large("drawing.arrangeBalloons", N_("Расставить\nпозиции"));
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
            sink.BeginGroup(N_("Документы"));
            sink.Large("drawing.specification", N_("Специфи-\nкация"));
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
        const bool spec = sheet.kind == dw::SheetKind::Specification;
        if (TreeRow(ui, "##sheet", spec ? "ui-list" : "doc-drawing", SheetTitle(sheet), x, y, width, active, false, &twice) && !active) {
            d.activeSheet = sheet.id;
            d.activePage = 0;
            d.zoom = 0;
        }
        if (twice) app.Later([&app] { OpenSheetDialog(app); });
        y += ui.S(30);
        if (active && spec) {
            // Its pages, as many as the rows take.
            const dw::SheetContent content = DrawingViews::Of(d).Content(app.workspace, d);
            const std::vector<dw::Page> pages = dw::PagesOf(d.drawing, SheetIndex(d), content);
            for (std::size_t i = 0; i < pages.size(); ++i) {
                ImGui::PushID(static_cast<int>(i));
                if (TreeRow(ui, "##page", nullptr, Trf("Лист {} из {}", i + 1, pages.size()), x + ui.S(24), y, width - ui.S(24),
                            d.activePage == static_cast<int>(i), false))
                    d.activePage = static_cast<int>(i);
                y += ui.S(30);
                ImGui::PopID();
            }
        }
        if (active && !spec) {
            // Its views; what this version does not know is counted, kept as it is.
            const std::vector<dw::View> views = dw::ViewsOf(sheet);
            for (const dw::View& v : views) {
                ImGui::PushID(static_cast<int>(v.id.value));
                const bool picked = d.selectedView == v.id;
                const float indent = ui.S(v.parent ? 40 : 24);
                if (TreeRow(ui, "##view", v.direction == dw::ViewDirection::Iso ? "ui-cube" : "ui-frame", ViewTitle(v), x + indent, y,
                            width - indent, picked, false)) {
                    d.selectedView = v.id;
                    d.selectedAnnotation = {};
                }
                if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                    d.selectedView = v.id;
                    d.selectedAnnotation = {};
                    ImGui::OpenPopup("##view-menu");
                }
                y += ui.S(30);
                // Its dimensions: the value now; red when its edges are lost (ADR-0012).
                for (const dw::Dimension& dim : dw::DimensionsOf(sheet)) {
                    if (dim.view != v.id) continue;
                    ImGui::PushID(static_cast<int>(dim.id.value));
                    const auto m = dw::MeasureOf(v, dim, DrawingViews::Of(d).Edges(app.workspace, d, v, dim.edges));
                    const std::string text = m ? dw::DimensionText(dim, *m) : Tr("Размер: ребро пропало");
                    const float in = indent + ui.S(16);
                    if (TreeRow(ui, "##dim", "sk-dimension", text, x + in, y, width - in, d.selectedAnnotation == dim.id, false, nullptr, !m)) {
                        d.selectedAnnotation = dim.id;
                        d.selectedView = {};
                    }
                    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                        d.selectedAnnotation = dim.id;
                        d.selectedView = {};
                        ImGui::OpenPopup("##dim-menu");
                    }
                    y += ui.S(30);
                    ImGui::PopID();
                }
                // Its positions: the number now; red when the component is gone.
                for (const dw::Balloon& b : dw::BalloonsOf(sheet)) {
                    if (b.view != v.id) continue;
                    ImGui::PushID(static_cast<int>(b.id.value));
                    const auto t = DrawingViews::Of(d).BalloonTarget(app.workspace, d, v, b);
                    const std::string text = t ? Trf("Позиция {}", t->number) : Tr("Позиция: компонента нет");
                    const float in = indent + ui.S(16);
                    if (TreeRow(ui, "##balloon", "ui-marker", text, x + in, y, width - in, d.selectedAnnotation == b.id, false, nullptr, !t)) {
                        d.selectedAnnotation = b.id;
                        d.selectedView = {};
                    }
                    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                        d.selectedAnnotation = b.id;
                        d.selectedView = {};
                        ImGui::OpenPopup("##balloon-menu");
                    }
                    y += ui.S(30);
                    ImGui::PopID();
                }
                ImGui::PopID();
            }
            const std::size_t newer = sheet.views.size() - views.size() + sheet.annotations.size() - dw::DimensionsOf(sheet).size() - dw::BalloonsOf(sheet).size();
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
    DimensionMenu(d);
    BalloonMenu(d);
    ImGui::SetCursorScreenPos({pos.x, y});
    ImGui::Dummy({1, ui.S(8)});
    EndArea();
}

// --- the sheet ------------------------------------------------------------------------

std::string DrawingSheetCaption(const DrawingState& d) {
    if (d.Sheet().kind == dw::SheetKind::Specification) return Trf("{}, лист {}, А4", d.Sheet().name, d.activePage + 1);
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
    const dw::SheetContent content = views.Content(app.workspace, d);
    const dw::Page page = CurrentPage(d, content);
    const dw::SheetLayout layout = dw::LayoutPage(d.drawing, page, content);
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
    DimensionDrag& dimDrag = DimDrag();
    DimensionTool& tool = Tool();
    if (tool.active && (sheet.kind != dw::SheetKind::Drawing || (hovered && ImGui::IsKeyPressed(ImGuiKey_Escape)))) {
        tool.active = false;
        tool.Reset();
    }
    // A dimension being made: edges picked on a view, then its place.
    std::optional<dw::Dimension> phantom;
    std::optional<dw::View>      phantomView;
    if (tool.active && hovered) {
        const Vec2 at = view.ToSheet(io.MousePos);
        const ObjectId id = tool.view ? tool.view : viewAt(at);
        const auto v = std::find_if(onSheet.begin(), onSheet.end(), [&](const dw::View& x) { return x.id == id; });
        if (v != onSheet.end()) {
            std::optional<dw::Measure> measure;
            if (tool.CanPlace()) {
                phantom = ToolDimension(tool, *v, at);
                phantomView = *v;
                measure = dw::MeasureOf(*v, *phantom, std::vector<std::optional<dw::ModelEdge>>(tool.edges.begin(), tool.edges.end()));
                if (!measure) phantom.reset();
            }
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                bool took = false;
                if (tool.edges.size() < tool.Needs()) {
                    dw::Json ref;
                    const auto e = views.EdgeAt(app.workspace, d, *v, at, 6.0 / d.zoom, ref);
                    const bool fits = e && (tool.kind == dw::DimensionKind::Linear ||
                                            (tool.kind == dw::DimensionKind::Angle ? e->kind == dw::ModelEdge::Kind::Line
                                                                                   : e->kind == dw::ModelEdge::Kind::Circle));
                    if (fits) {
                        tool.view = v->id;
                        tool.refs.push_back(std::move(ref));
                        tool.edges.push_back(*e);
                        took = true;
                    } else if (e) {
                        app.notes.Info(tool.kind == dw::DimensionKind::Angle ? Tr("Угол ставится между прямыми рёбрами")
                                                                             : Tr("Нужна окружность или дуга"));
                        took = true;
                    }
                }
                if (!took && phantom) {
                    const dw::Dimension made = *phantom;
                    const ObjectId sheetId = d.Sheet().id;
                    ObjectId madeId;
                    d.Change(Tr("Размер"), [&](dw::Drawing& dr) {
                        for (dw::Sheet& s : dr.sheets)
                            if (s.id == sheetId) {
                                dw::Dimension x = made;
                                x.id = dr.Ids().Next();
                                madeId = x.id;
                                s.annotations.push_back(dw::MakeDimensionItem(x));
                            }
                    });
                    d.selectedAnnotation = madeId;
                    d.selectedView = {};
                    tool.Reset();   // the command stays on for the next dimension
                    phantom.reset();
                }
            }
        }
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {   // a step back: the picked edges, then the command
            if (tool.edges.empty()) tool.active = false;
            tool.Reset();
            phantom.reset();
        }
    }
    // A section being made: the line's ends on a view, then its place.
    SectionTool& cutter = Cutter();
    if (cutter.active && (sheet.kind != dw::SheetKind::Drawing || (hovered && ImGui::IsKeyPressed(ImGuiKey_Escape)))) {
        cutter.active = false;
        cutter.Reset();
    }
    std::optional<dw::View> sectionGhost, sectionParent;
    std::optional<std::pair<Vec2, Vec2>> cutGhost;   // the line on the sheet while its second end is picked
    if (cutter.active && hovered) {
        const Vec2 at = view.ToSheet(io.MousePos);
        // The line starts outside the contour as a rule: the view nearest within 25 mm, if none is under the cursor.
        ObjectId id = cutter.parent && !cutter.points.empty() ? cutter.parent : viewAt(at);
        if (!id) {
            double best = 25;
            for (std::size_t i = 0; i < onSheet.size(); ++i) {
                const Box2& b = boxes[i];
                const double dx = std::max({b.min.x - at.x, 0.0, at.x - b.max.x}), dy = std::max({b.min.y - at.y, 0.0, at.y - b.max.y});
                if (const double dist = std::hypot(dx, dy); dist < best && !onSheet[i].section) {
                    best = dist;
                    id = onSheet[i].id;
                }
            }
        }
        const auto v = std::find_if(onSheet.begin(), onSheet.end(), [&](const dw::View& x) { return x.id == id; });
        if (v != onSheet.end() && !v->section) {
            const Vec2 here = dw::ViewPointOf(*v, at);
            const double s = v->scale.Value();
            if (cutter.points.size() == 1) {
                const Vec2 b = SnapCut(cutter.points[0], here, io.KeyCtrl);
                cutGhost = std::pair{v->at + cutter.points[0] * s, v->at + b * s};
            } else if (cutter.points.size() == 2) {
                sectionGhost = SectionAt(app, d, *v, cutter.points[0], cutter.points[1], at);
                sectionParent = *v;
            }
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                if (cutter.points.empty()) {
                    cutter.parent = v->id;
                    cutter.points.push_back(here);
                } else if (cutter.points.size() == 1) {
                    const Vec2 b = SnapCut(cutter.points[0], here, io.KeyCtrl);
                    if ((b - cutter.points[0]).Length() * s > 2) cutter.points.push_back(b);
                } else if (sectionGhost) {
                    dw::View made = *sectionGhost;
                    const ObjectId sheetId = d.Sheet().id;
                    d.Change(Tr("Разрез"), [&](dw::Drawing& dr) {
                        for (dw::Sheet& sh : dr.sheets)
                            if (sh.id == sheetId) {
                                made.id = dr.Ids().Next();
                                sh.views.push_back(dw::MakeViewItem(made));
                            }
                    });
                    d.selectedView = made.id;
                    d.selectedAnnotation = {};
                    cutter.active = false;
                    cutter.Reset();
                    sectionGhost.reset();
                }
            }
        }
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            if (cutter.points.empty()) cutter.active = false;
            cutter.Reset();
            sectionGhost.reset();
            cutGhost.reset();
        }
    }
    // A position being placed: the component's dot, then the shelf.
    BalloonTool& pointer = Pointer();
    if (pointer.active && (sheet.kind != dw::SheetKind::Drawing || (hovered && ImGui::IsKeyPressed(ImGuiKey_Escape)))) {
        pointer.active = false;
        pointer.Reset();
    }
    std::optional<dw::Balloon> balloonGhost;
    std::optional<dw::View>    balloonView;
    if (pointer.active && hovered) {
        const Vec2 at = view.ToSheet(io.MousePos);
        const ObjectId id = pointer.picked ? pointer.view : viewAt(at);
        const auto v = std::find_if(onSheet.begin(), onSheet.end(), [&](const dw::View& x) { return x.id == id; });
        if (v != onSheet.end()) {
            if (pointer.picked) {
                balloonGhost = dw::Balloon{{}, v->id, pointer.component, pointer.point, dw::ViewPointOf(*v, at)};
                balloonView = *v;
            }
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                if (!pointer.picked) {
                    if (const auto hit = ComponentAt(app, d, *v, at)) {
                        pointer.view = v->id;
                        pointer.component = hit->first;
                        pointer.point = hit->second;
                        pointer.picked = true;
                    } else if (views.Parts(app.workspace, d, v->model).empty()) {
                        app.notes.Info(Tr("Позиции ставятся на виды сборки"));
                    }
                } else if (balloonGhost) {
                    dw::Balloon made = *balloonGhost;
                    const ObjectId sheetId = d.Sheet().id;
                    d.Change(Tr("Позиция"), [&](dw::Drawing& dr) {
                        for (dw::Sheet& sh : dr.sheets)
                            if (sh.id == sheetId) {
                                made.id = dr.Ids().Next();
                                sh.annotations.push_back(dw::MakeBalloonItem(made));
                            }
                    });
                    d.selectedAnnotation = made.id;
                    d.selectedView = {};
                    pointer.Reset();   // stays on for the next one
                    balloonGhost.reset();
                }
            }
        }
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            if (!pointer.picked) pointer.active = false;
            pointer.Reset();
            balloonGhost.reset();
        }
    }
    const bool busy = tool.active || cutter.active || pointer.active;
    if (!busy && hovered && (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right))) {
        const Vec2 at = view.ToSheet(io.MousePos);
        // A dimension over the views is picked first.
        dimDrag = {};
        d.selectedAnnotation = {};
        for (std::size_t i = layout.hits.size(); i-- > 0;) {
            const Box2& b = layout.hits[i].box;
            if (at.x >= b.min.x - 1 && at.x <= b.max.x + 1 && at.y >= b.min.y - 1 && at.y <= b.max.y + 1) {
                d.selectedAnnotation = layout.hits[i].id;
                break;
            }
        }
        if (d.selectedAnnotation) {
            d.selectedView = {};
            if (const dw::Item* item = dw::FindAnnotation(d.Sheet(), d.selectedAnnotation); item && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                if (const auto place = AnnotationPlace(*item))
                    for (const dw::View& v : onSheet)
                        if (v.id == place->first) dimDrag = {d.selectedAnnotation, at, place->second, v.scale.Value(), false};
        }
    }
    if (dimDrag.id && ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const Vec2 delta = view.ToSheet(io.MousePos) - dimDrag.start;
        if (std::abs(delta.x) + std::abs(delta.y) > 0) dimDrag.moved = true;
        if (dw::Item* item = dw::FindAnnotation(d.Sheet(), dimDrag.id)) MoveAnnotation(*item, dimDrag.from + delta / dimDrag.scale);
    }
    if (dimDrag.id && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        if (dimDrag.moved)
            if (dw::Item* item = dw::FindAnnotation(d.Sheet(), dimDrag.id))
                if (const auto place = AnnotationPlace(*item)) {
                    // One undo step: back where it was, then the change.
                    const Vec2 to = place->second;
                    const ObjectId id = dimDrag.id, sheetId = d.Sheet().id;
                    MoveAnnotation(*item, dimDrag.from);
                    d.Change(Tr("Сдвинуть обозначение"), [&](dw::Drawing& dr) {
                        for (dw::Sheet& s : dr.sheets)
                            if (s.id == sheetId)
                                if (dw::Item* it = dw::FindAnnotation(s, id)) MoveAnnotation(*it, to);
                    });
                }
        dimDrag = {};
    }
    if (!busy && !d.selectedAnnotation && hovered &&
        (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right))) {
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
    } else if (!dimDrag.id && ImGui::IsItemActive() && (io.MouseDelta.x != 0 || io.MouseDelta.y != 0)) {
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
    // Right click on a view or a dimension (not a drag of the sheet): its menu.
    const bool rightClick = hovered && !busy && ImGui::IsMouseReleased(ImGuiMouseButton_Right) &&
                            ImGui::GetMouseDragDelta(ImGuiMouseButton_Right).x == 0 && ImGui::GetMouseDragDelta(ImGuiMouseButton_Right).y == 0;
    if (rightClick && d.selectedAnnotation) {
        const dw::Item* item = dw::FindAnnotation(d.Sheet(), d.selectedAnnotation);
        ImGui::OpenPopup(item && dw::ReadBalloon(*item) ? "##balloon-menu" : "##dim-menu");
    }
    else if (rightClick && d.selectedView) ImGui::OpenPopup("##view-menu");
    const bool deleteKey = (hovered || ImGui::IsWindowFocused()) && !ImGui::IsAnyItemActive() && ImGui::IsKeyPressed(ImGuiKey_Delete);
    if (deleteKey && d.selectedAnnotation) DeleteAnnotation(d, d.selectedAnnotation);
    else if (deleteKey && d.selectedView) DeleteView(d, d.selectedView);
    // Double click on the title block: fill it in (as in КОМПАС).
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        const Vec2 at = view.ToSheet(io.MousePos);
        if (at.x >= layout.titleBlock.min.x && at.x <= layout.titleBlock.max.x && at.y >= layout.titleBlock.min.y &&
            at.y <= layout.titleBlock.max.y)
            app.Later([&app] { OpenSheetDialog(app); });
    }

    ImGui::PushClipRect(pos, {pos.x + size.x, pos.y + size.y}, true);
    // Drawn from the layout of this frame's start; a view being dragged is drawn where it is now.
    const dw::SheetLayout now = drag.moved || dimDrag.moved ? dw::LayoutPage(d.drawing, page, content) : layout;
    DrawSheet(dl, ui, view, now);
    // The picked dimension framed; the one being made, and its edges, in the accent colour.
    for (const dw::Hit& h : now.hits)
        if (h.id == d.selectedAnnotation) {
            const ImVec2 a = view.ToScreen({h.box.min.x - 1, h.box.max.y + 1}), z = view.ToScreen({h.box.max.x + 1, h.box.min.y - 1});
            dl->AddRect(a, z, ui::ToU32(c.accent), ui.S(2), 0, ui.S(1.5f));
        }
    if (tool.active && tool.view)
        for (const dw::View& v : onSheet)
            if (v.id == tool.view)
                for (const dw::ModelEdge& e : tool.edges) DrawPickedEdge(dl, view, v, e, ui::ToU32(c.accent), ui.S(3));
    if (balloonGhost && balloonView) {
        dw::SheetLayout ghost;
        dw::AddBalloon(ghost, *balloonView, *balloonGhost, views.BalloonTarget(app.workspace, d, *balloonView, *balloonGhost));
        const ImU32 accent = ui::ToU32(c.accent);
        for (const dw::Polyline& p : ghost.polylines) DrawPolyline(dl, view, p, accent, 1.0f, 1.0f);
        for (const dw::Fill& f : ghost.fills) DrawFill(dl, view, f, accent);
        for (const dw::Label& l : ghost.labels) DrawLabel(dl, ui.fonts.body, view, l, accent);
    }
    if (cutGhost) {
        const float w = std::max(2.0f, static_cast<float>(0.7 * view.zoom));
        dl->AddLine(view.ToScreen(cutGhost->first), view.ToScreen(cutGhost->second), ui::ToU32(c.accent), w);
    }
    if (sectionGhost && sectionParent) {
        // The cutting line with its arrows on the parent, and where the section goes.
        dw::SheetLayout ghost;
        dw::AddCuttingLine(ghost, *sectionParent, *sectionGhost);
        const ImU32 accent = ui::ToU32(c.accent);
        const float thick = std::max(1.5f, static_cast<float>(0.5 * view.zoom));
        for (const dw::Stroke& st : ghost.strokes) dl->AddLine(view.ToScreen(st.a), view.ToScreen(st.b), accent, thick);
        for (const dw::Polyline& p : ghost.polylines) DrawPolyline(dl, view, p, accent, thick, 1.0f);
        for (const dw::Fill& f : ghost.fills) DrawFill(dl, view, f, accent);
        for (const dw::Label& l : ghost.labels) DrawLabel(dl, ui.fonts.body, view, l, accent);
        if (const auto bounds = views.ModelBounds(app.workspace, d, sectionGhost->model)) {
            const Box2 e = dw::Extent(*sectionGhost, BoxOf(*bounds));
            const double s = sectionGhost->scale.Value();
            const ImVec2 p0 = view.ToScreen(sectionGhost->at + Vec2{e.min.x, e.max.y} * s);
            const ImVec2 p1 = view.ToScreen(sectionGhost->at + Vec2{e.max.x, e.min.y} * s);
            dl->AddRect(p0, p1, accent, 0, 0, 1.5f);
        }
    }
    if (phantom && phantomView) {
        dw::SheetLayout ghost;
        ghost.width = now.width;
        ghost.height = now.height;
        dw::AddDimension(ghost, *phantomView, *phantom,
                         dw::MeasureOf(*phantomView, *phantom, std::vector<std::optional<dw::ModelEdge>>(tool.edges.begin(), tool.edges.end())));
        const ImU32 accent = ui::ToU32(c.accent);
        const float thin = std::max(1.0f, static_cast<float>(0.18 * view.zoom));
        for (const dw::Polyline& p : ghost.polylines) DrawPolyline(dl, view, p, accent, thin, thin);
        for (const dw::Fill& f : ghost.fills) DrawFill(dl, view, f, accent);
        for (const dw::Label& l : ghost.labels) DrawLabel(dl, ui.fonts.body, view, l, accent);
    }
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
    const std::size_t newer = sheet.views.size() - onSheet.size() + sheet.annotations.size() - dw::DimensionsOf(sheet).size() - dw::BalloonsOf(sheet).size();
    std::string note;
    if (tool.active) note = Tr(ToolPrompt(tool));
    else if (pointer.active)
        note = pointer.picked ? Tr("Щелчок — место полки с номером (Esc — выйти)") : Tr("Позиция: щелчок по детали на виде сборки (Esc — выйти)");
    else if (cutter.active)
        note = cutter.points.empty()      ? Tr("Разрез: щелчок — начало линии разреза на виде (Esc — выйти)")
               : cutter.points.size() == 1 ? Tr("Конец линии разреза. Почти горизонтальная или вертикальная выравнивается; Ctrl — под любым углом")
                                           : Tr("Щелчок — место разреза: взгляд направлен в его сторону");
    else if (newer > 0) note = Trf("На листе есть объекты из более новой версии BreezeCAD ({}): они сохранятся без изменений", newer);
    else if (onSheet.empty() && sheet.kind == dw::SheetKind::Drawing) note = Tr("«Стандартные виды» на вкладке «Виды» — главный вид, сверху и слева по детали. Двойной щелчок по надписи — заполнить её");
    else if (views.Busy()) note = Tr("Виды считаются…");
    if (!note.empty()) {
        const ImVec2 ns = ui.fonts.body->CalcTextSizeA(ui.S(12), FLT_MAX, 0, note.c_str());
        const ImVec2 n0{pos.x + (size.x - ns.x) / 2 - ui.S(14), pos.y + ui.S(12)};
        dl->AddRectFilled(n0, {n0.x + ns.x + ui.S(28), n0.y + ns.y + ui.S(14)}, ui::ToU32(c.floatingButton), ui.S(14));
        dl->AddText(ui.fonts.body, ui.S(12), {n0.x + ui.S(14), n0.y + ui.S(7)}, ui::ToU32(c.textSecondary), note.c_str());
    }

    ViewMenu(d);
    DimensionMenu(d);
    BalloonMenu(d);
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

    const bool spec = dlg.edit.kind == dw::SheetKind::Specification;   // always А4 upright (ГОСТ 2.106)
    if (!spec) {
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
    }

    ImGui::Dummy({0, ui.S(6)});
    ImGui::PushStyleColor(ImGuiCol_Text, ui::ToVec4(c.textSecondary));
    ImGui::TextUnformatted(spec ? Tr("Основная надпись (ГОСТ 2.104, форма 2)") : Tr("Основная надпись (ГОСТ 2.104, форма 1)"));
    ImGui::TextUnformatted(Tr("Серым — из модели: пустое поле берёт значение из её свойств и меняется вместе с ней"));
    ImGui::PopStyleColor();
    const dw::Fields automatic = DrawingViews::Of(*d).AutoFields(app.workspace, *d, dlg.edit);
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
        const auto hint = automatic.find(f.key);
        if (ImGui::InputTextWithHint("##value", hint != automatic.end() ? hint->second.c_str() : "", &value)) {
            // Emptied: back to the model's value.
            if (value.empty()) dlg.edit.title.fields.erase(f.key);
            else dlg.edit.title.fields[f.key] = value;
        }
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
