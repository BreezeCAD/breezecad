#include "app/drawing/DrawingViews.h"

#include "app/Session.h"
#include "app/drawing/DrawingState.h"
#include "core/I18n.h"
#include "core/Monitor.h"
#include "core/PathText.h"
#include "core/Profile.h"
#include "document/BzcFile.h"
#include "document/Document.h"
#include "drawing/SheetLayout.h"
#include "kernel/Model.h"
#include "kernel/drawing/ViewProjection.h"

#include <algorithm>
#include <array>
#include <format>
#include <system_error>

namespace bz::app {

namespace dw = bz::drawing;
namespace fs = std::filesystem;

namespace {

constexpr std::chrono::milliseconds kFileCheckEvery{700};   // how often a model's file is looked at

fs::path Normal(const fs::path& p) {
    std::error_code ec;
    const fs::path full = fs::weakly_canonical(p, ec);
    return ec ? p.lexically_normal() : full;
}

const dw::ModelRef* FindModel(const dw::Drawing& d, ObjectId id) {
    for (const dw::ModelRef& m : d.models)
        if (m.id == id) return &m;
    return nullptr;
}

/// The part open in a tab under `file`; nullptr when it is not open.
const Session* OpenPart(const OpenSessions* open, const fs::path& file) {
    if (!open) return nullptr;
    const fs::path wanted = Normal(file);
    for (const auto& s : *open)
        if (s && !s->drawing && !s->assembly && !s->path.empty() && Normal(s->path) == wanted) return s.get();
    return nullptr;
}

} // namespace

/// One model of the drawing as the views see it.
struct DrawingViewCache::Source {
    fs::path file;
    // Where the model was taken from and in what state: a change of any of these means «project again».
    const Session* open = nullptr;        // a tab; nullptr — the file
    std::uint64_t  revision = 0;          // the tab's model revision
    fs::file_time_type written{};         // the file's time and size
    std::uintmax_t     size = 0;
    bool               known = false;     // the fields above describe what is in `lines`
    std::chrono::steady_clock::time_point checked{};

    std::unique_ptr<doc::Document> doc;   // read from the file (not used for a tab)
    std::unique_ptr<kernel::Model> model;
    std::string problem;
    std::map<dw::ViewProjection, std::shared_ptr<const dw::ViewLines>> lines;
};

DrawingViewCache::DrawingViewCache() = default;
DrawingViewCache::~DrawingViewCache() = default;

fs::path ModelFile(const DrawingState& d, const dw::ModelRef& model) {
    const fs::path kept = PathFromUtf8(model.path);
    if (kept.is_absolute() || d.savedAt.empty()) return kept;
    return d.savedAt.parent_path() / kept;
}

DrawingViewCache::Source* DrawingViewCache::Update(const DrawingState& d, const OpenSessions* open, ObjectId model) {
    const dw::ModelRef* ref = FindModel(d.drawing, model);
    if (!ref) return nullptr;
    std::unique_ptr<Source>& slot = mySources[model];
    if (!slot) slot = std::make_unique<Source>();
    Source& s = *slot;
    const fs::path file = ModelFile(d, *ref);
    const auto forget = [&] {
        s.known = false;
        s.lines.clear();
        s.problem.clear();
    };
    if (file != s.file) {
        s.file = file;
        s.doc.reset();
        s.model.reset();
        forget();
    }
    if (file.extension() != ".bzc") {
        s.problem = N_("Виды сборок появятся в следующих версиях");
        return &s;
    }

    // A part open in a tab: its model as it is now.
    if (const Session* tab = OpenPart(open, file)) {
        const std::uint64_t revision = tab->model.Revision();
        if (!s.known || s.open != tab || s.revision != revision) {
            forget();
            s.open     = tab;
            s.revision = revision;
            s.known    = true;
            s.doc.reset();
            s.model.reset();
        }
        return &s;
    }

    // A part on disk: read and built here; looked at again from time to time.
    const auto now = std::chrono::steady_clock::now();
    if (s.known && !s.open && now - s.checked < kFileCheckEvery) return &s;
    s.checked = now;
    std::error_code ec;
    const fs::file_time_type written = fs::last_write_time(file, ec);
    const std::uintmax_t size = ec ? 0 : fs::file_size(file, ec);
    if (ec) {
        if (!s.known || s.open || s.problem.empty()) {
            forget();
            s.doc.reset();
            s.model.reset();
        }
        s.open    = nullptr;
        s.known   = true;
        s.written = {};
        s.size    = 0;
        s.problem = std::format(N_("Модель не найдена: {}"), ToUtf8(file.filename()));
        return &s;
    }
    if (s.known && !s.open && s.problem.empty() && written == s.written && size == s.size) return &s;
    forget();
    s.open    = nullptr;
    s.written = written;
    s.size    = size;
    s.known   = true;
    // Reading and building a part takes time: work, not a hang.
    const mon::Waiting waiting;
    const prof::Scope scope("drawing model");
    Expected<doc::Document> doc = doc::LoadBzc(file);
    if (!doc) {
        s.doc.reset();
        s.model.reset();
        s.problem = doc.GetError().message;
        return &s;
    }
    s.doc   = std::make_unique<doc::Document>(std::move(*doc));
    s.model = std::make_unique<kernel::Model>();
    s.model->Rebuild(*s.doc);
    return &s;
}

std::shared_ptr<const dw::ViewLines> DrawingViewCache::Project(Source& s, dw::ViewProjection projection) {
    if (!s.problem.empty()) return nullptr;
    if (const auto it = s.lines.find(projection); it != s.lines.end()) return it->second;
    const kernel::Model* model = s.open ? &s.open->model : s.model.get();
    if (!model) return nullptr;
    const mon::Waiting waiting;
    const prof::Scope scope("drawing view");
    Expected<dw::ViewLines> lines = kernel::ProjectView(*model, kernel::AxesOf(projection));
    if (!lines) {
        s.problem = lines.GetError().message;
        return nullptr;
    }
    if (lines->Empty()) {
        s.problem = N_("В детали нет тел: вид пуст");
        return nullptr;
    }
    auto kept = std::make_shared<const dw::ViewLines>(std::move(*lines));
    s.lines[projection] = kept;
    return kept;
}

void DrawingViewCache::Refresh(const DrawingState& d, const OpenSessions* open) {
    dw::ViewImages images;
    std::map<ObjectId, bool> used;
    for (const dw::Sheet& sheet : d.drawing.sheets)
        for (const dw::ModelView& view : dw::ModelViewsOf(sheet)) {
            used[view.model] = true;
            Source* source = Update(d, open, view.model);
            if (!source) continue;
            if (auto lines = Project(*source, view.projection)) images[view.id] = std::move(lines);
        }
    // A model no view shows any more is let go: no need to keep it built.
    std::erase_if(mySources, [&](const auto& entry) { return !used.contains(entry.first); });
    if (images != myImages) {
        myImages = std::move(images);
        ++myGeneration;
    }
}

void DrawingViewCache::CheckFilesNow() {
    for (auto& [id, source] : mySources) source->checked = {};
}

std::string DrawingViewCache::Problem(ObjectId model) const {
    const auto it = mySources.find(model);
    return it == mySources.end() ? std::string{} : it->second->problem;
}

Expected<std::shared_ptr<const dw::ViewLines>> DrawingViewCache::Lines(const DrawingState& d, const OpenSessions* open, ObjectId model,
                                                                        dw::ViewProjection projection) {
    Source* source = Update(d, open, model);
    if (!source) return Error{N_("В чертеже нет такой модели")};
    auto lines = Project(*source, projection);
    if (!lines) return Error{source->problem.empty() ? std::string(N_("Не удалось построить вид")) : source->problem};
    return lines;
}

DrawingViewsState& ViewsOf(DrawingState& d) {
    if (!d.views) d.views = std::make_shared<DrawingViewsState>();
    return *d.views;
}

const std::vector<dw::PlacedView>& PlacedViews(DrawingState& d) {
    DrawingViewsState& st = ViewsOf(d);
    const bool dragging = st.dragged.IsValid() && st.dragMoved;
    const bool same = st.placedRevision == d.Revision() && st.placedGeneration == st.cache.Generation() && st.placedSheet == d.Sheet().id &&
                      st.placedDragging == dragging && (!dragging || st.placedDragAt == st.dragAt);
    if (same) return st.placed;
    if (dragging) {
        dw::Sheet preview = d.Sheet();   // the sheet as it will be when the button is let go
        dw::MoveViewTo(preview, st.dragged, st.dragAt);
        st.placed = dw::LayoutViews(preview, st.cache.Images());
    } else {
        st.placed = dw::LayoutViews(d.Sheet(), st.cache.Images());
    }
    st.placedRevision   = d.Revision();
    st.placedGeneration = st.cache.Generation();
    st.placedSheet      = d.Sheet().id;
    st.placedDragging   = dragging;
    st.placedDragAt     = st.dragAt;
    return st.placed;
}

const dw::ViewImages& DrawingViewImages(DrawingState& d, const OpenSessions* open) {
    DrawingViewCache& cache = ViewsOf(d).cache;
    cache.CheckFilesNow();   // an export shows the models as they are at this moment
    cache.Refresh(d, open);
    return cache.Images();
}

// --- changes ----------------------------------------------------------------------------

namespace {

/// The sheet of `dr` that `d` shows (inside a Change the drawing is `dr`).
dw::Sheet& ShownSheet(dw::Drawing& dr, const DrawingState& d) {
    const ObjectId id = d.Sheet().id;
    for (dw::Sheet& s : dr.sheets)
        if (s.id == id) return s;
    return dr.sheets.front();
}

/// Ids of views are not known to the drawing's generator when the file gave no "nextId".
void ReserveViewIds(dw::Drawing& dr) {
    for (const dw::Sheet& sheet : dr.sheets)
        for (const dw::ModelView& view : dw::ModelViewsOf(sheet)) dr.Ids().Reserve(view.id);
}

/// The main view `view` belongs to: itself, or its parent when that is on the sheet.
ObjectId MainOf(const dw::Sheet& sheet, const dw::ModelView& view) {
    if (!view.parent.IsValid()) return view.id;
    return dw::FindViewItem(sheet, view.parent) ? view.parent : view.id;
}

} // namespace

Expected<ObjectId> AddStandardViews(DrawingState& d, const OpenSessions* open) {
    const auto part = std::find_if(d.drawing.models.begin(), d.drawing.models.end(),
                                   [](const dw::ModelRef& m) { return PathFromUtf8(m.path).extension() == ".bzc"; });
    if (part == d.drawing.models.end()) {
        if (!d.drawing.models.empty()) return Error{N_("Виды сборок появятся в следующих версиях")};
        return Error{N_("В чертеже нет детали. Откройте деталь и выберите «Создать чертёж» в меню «Файл»")};
    }
    const ObjectId model = part->id;
    DrawingViewCache& cache = ViewsOf(d).cache;
    std::array<std::shared_ptr<const dw::ViewLines>, 3> lines;
    const std::array<dw::ViewProjection, 3> projections{dw::ViewProjection::Front, dw::ViewProjection::Top, dw::ViewProjection::Left};
    for (std::size_t i = 0; i < 3; ++i) {
        auto got = cache.Lines(d, open, model, projections[i]);
        if (!got) return got.GetError();
        lines[i] = *got;
    }
    const dw::SheetLayout layout = dw::LayoutSheet(d.Sheet(), d.SheetNumber(), static_cast<int>(d.drawing.sheets.size()));
    const std::optional<dw::StandardViewsPlan> plan = dw::PlanStandardViews(layout, *lines[0], *lines[1], *lines[2]);
    if (!plan) return Error{N_("Виды не помещаются на лист даже в самом мелком масштабе")};

    ObjectId main;
    d.Change(Tr("Стандартные виды"), [&](dw::Drawing& dr) {
        ReserveViewIds(dr);
        dw::Sheet& sheet = ShownSheet(dr, d);
        main = dr.Ids().Next();
        const std::array<Vec2, 3> at{plan->front, plan->top, plan->left};
        for (std::size_t i = 0; i < 3; ++i) {
            dw::ModelView view;
            view.id         = i == 0 ? main : dr.Ids().Next();
            view.model      = model;
            view.projection = projections[i];
            view.at         = at[i];
            if (i == 0) view.scale = plan->scale;
            else view.parent = main;
            sheet.views.push_back(dw::MakeViewItem(view));
        }
        // The title block shows the scale of the drawing; one the user wrote (even empty) is left alone.
        if (!sheet.title.fields.contains(dw::field::kScale)) sheet.title.fields[dw::field::kScale] = dw::ScaleText(plan->scale);
    });
    return main;
}

void MoveView(DrawingState& d, ObjectId id, Vec2 at) {
    d.Change(Tr("Переместить вид"), [&](dw::Drawing& dr) { dw::MoveViewTo(ShownSheet(dr, d), id, at); });
}

void SetViewScale(DrawingState& d, ObjectId id, dw::Scale scale) {
    d.Change(Tr("Масштаб вида"), [&](dw::Drawing& dr) {
        dw::Sheet& sheet = ShownSheet(dr, d);
        const dw::Item* picked = dw::FindViewItem(sheet, id);
        const std::optional<dw::ModelView> start = picked ? dw::ReadModelView(*picked) : std::nullopt;
        if (!start) return;
        dw::Item* item = dw::FindViewItem(sheet, MainOf(sheet, *start));
        std::optional<dw::ModelView> main = item ? dw::ReadModelView(*item) : std::nullopt;
        if (!main) return;
        const dw::Scale before = dw::PlaceOf(sheet, *main).scale;
        main->scale = scale;
        dw::StoreModelView(*item, *main);
        const auto shown = sheet.title.fields.find(dw::field::kScale);
        if (shown != sheet.title.fields.end() && shown->second == dw::ScaleText(before)) shown->second = dw::ScaleText(scale);
    });
}

void SetViewHiddenLines(DrawingState& d, ObjectId id, bool shown) {
    d.Change(shown ? Tr("Показать невидимые линии") : Tr("Скрыть невидимые линии"), [&](dw::Drawing& dr) {
        dw::Item* item = dw::FindViewItem(ShownSheet(dr, d), id);
        std::optional<dw::ModelView> view = item ? dw::ReadModelView(*item) : std::nullopt;
        if (!view) return;
        view->hiddenLines = shown;
        dw::StoreModelView(*item, *view);
    });
}

void RemoveView(DrawingState& d, ObjectId id) {
    d.Change(Tr("Удалить вид"), [&](dw::Drawing& dr) {
        std::erase_if(ShownSheet(dr, d).views, [&](const dw::Item& item) {
            const std::optional<dw::ModelView> view = dw::ReadModelView(item);
            return view && (view->id == id || view->parent == id);
        });
    });
    DrawingViewsState& state = ViewsOf(d);
    if (state.selected == id) state.selected = {};
}

} // namespace bz::app
