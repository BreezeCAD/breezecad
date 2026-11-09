#include "app/drawing/DrawingViews.h"

#include "app/Session.h"
#include "app/Workspace.h"
#include "app/drawing/DrawingState.h"
#include "core/I18n.h"
#include "core/PathText.h"

#include <chrono>
#include <format>

namespace bz::app {

namespace dw = bz::drawing;

/// A model read here from its file (not open in a tab).
struct DrawingViews::Source {
    std::unique_ptr<Session>        session;
    std::filesystem::file_time_type written{};
    std::chrono::steady_clock::time_point checked{};
    std::string                     problem;
};

DrawingViews::~DrawingViews() {
    if (myJob.valid()) myJob.wait();
}

DrawingViews& DrawingViews::Of(DrawingState& d) {
    if (!d.views) d.views = std::make_shared<DrawingViews>();
    return *d.views;
}

std::filesystem::path DrawingViews::ModelFile(const DrawingState& d, ObjectId model) {
    for (const dw::ModelRef& m : d.drawing.models)
        if (m.id == model) {
            const std::filesystem::path p = PathFromUtf8(m.path);
            if (p.is_absolute() || d.savedAt.empty()) return p.lexically_normal();
            return (d.savedAt.parent_path() / p).lexically_normal();
        }
    return {};
}

const kernel::Model* DrawingViews::ModelOf(Workspace& ws, DrawingState& d, ObjectId id, std::string& sourceKey, std::string& problem) {
    const std::filesystem::path file = ModelFile(d, id);
    if (file.empty()) {
        problem = Tr("Модели этого вида нет в чертеже");
        return nullptr;
    }
    if (file.extension() == ".bza") {
        problem = Tr("Виды сборки — в следующей версии");
        return nullptr;
    }
    // Open in a tab: as it is now, edits included.
    for (const auto& s : ws.sessions)
        if (!s->drawing && !s->assembly && !s->path.empty() && SameFile(s->path, file)) {
            sourceKey = std::format("tab:{}:{}", ToUtf8(file), s->model.Revision());
            return &s->model;
        }
    // Else read from the file; again when it is written anew (looked at every two seconds).
    const std::string name = ToUtf8(file);
    std::unique_ptr<Source>& src = mySources[name];
    if (!src) src = std::make_unique<Source>();
    const auto now = std::chrono::steady_clock::now();
    if (!src->session || now - src->checked > std::chrono::seconds(2)) {
        src->checked = now;
        std::error_code ec;
        const auto written = std::filesystem::last_write_time(file, ec);
        if (ec) {
            src->session.reset();
            src->problem = Trf("Нет файла модели «{}»", ToUtf8(file.filename()));
        } else if (!src->session || written != src->written) {
            src->written = written;
            Expected<std::unique_ptr<Session>> loaded = Workspace::LoadSession(file, nullptr);
            if (!loaded) {
                src->session.reset();
                src->problem = Trf("Модель «{}» не открылась: {}", ToUtf8(file.filename()), Tr(loaded.GetError().message));
            } else if ((*loaded)->assembly || (*loaded)->drawing) {
                src->session.reset();
                src->problem = Tr("Виды сборки — в следующей версии");
            } else {
                src->session = std::move(*loaded);
                src->session->RebuildNow();
                src->problem.clear();
            }
        }
    }
    if (!src->session) {
        problem = src->problem;
        return nullptr;
    }
    sourceKey = std::format("file:{}:{}:{}", name, src->written.time_since_epoch().count(), src->session->model.Revision());
    return &src->session->model;
}

void DrawingViews::Start(ObjectId view, const std::string& key, const kernel::ViewShapes& shapes, dw::ViewDirection direction) {
    const dw::ViewBasis b = dw::BasisOf(direction);
    myJobView = view;
    myJobKey = key;
    myJob = std::async(std::launch::async, [shapes, b] { return shapes.Project(b.eye, b.up); });
}

void DrawingViews::Poll() {
    if (!myJob.valid() || myJob.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    Expected<kernel::ProjectedLines> result = myJob.get();
    Entry& e = myEntries[myJobView];
    e.key = myJobKey;
    if (!result) {
        e.ready = false;
        e.lines = {};
        e.problem = Tr(result.GetError().message);
        return;
    }
    e.ready = true;
    e.problem.clear();
    e.lines.visible = std::move(result->visible);
    e.lines.hidden = std::move(result->hidden);
    e.lines.marks.clear();
    for (const auto& m : result->marks) e.lines.marks.push_back({m.center, m.radius});
    e.lines.axes.clear();
    for (const auto& a : result->axes) e.lines.axes.push_back({a.a, a.b});
}

const dw::ViewLines* DrawingViews::Lines(Workspace& ws, DrawingState& d, const dw::View& view) {
    Poll();
    Entry& e = myEntries[view.id];
    std::string sourceKey, problem;
    const kernel::Model* model = ModelOf(ws, d, view.model, sourceKey, problem);
    if (!model) {
        e.ready = false;
        e.key.clear();
        e.problem = problem;
        return nullptr;
    }
    const std::string key = sourceKey + "|" + std::string(dw::DirectionId(view.direction));
    if (e.key == key) return e.ready ? &e.lines : nullptr;
    const bool computing = myJob.valid() && myJobView == view.id && myJobKey == key;
    if (!computing && !myJob.valid()) {
        // The bodies copied once per model state, shared by its views.
        const std::string file = sourceKey.substr(0, sourceKey.rfind(':'));
        auto& [shapesKey, shapes] = myShapes[file];
        if (shapesKey != sourceKey) {
            shapesKey = sourceKey;
            shapes = kernel::ViewShapes::Of(*model);
        }
        if (shapes.Empty()) {
            e.ready = false;
            e.key = key;
            e.problem = Tr("В модели нет тел");
            return nullptr;
        }
        e.problem.clear();
        Start(view.id, key, shapes, view.direction);
    }
    // Meanwhile the lines it had (a model being edited), if any.
    return e.ready ? &e.lines : nullptr;
}

std::string DrawingViews::Problem(ObjectId view) const {
    const auto it = myEntries.find(view);
    if (it == myEntries.end() || (!it->second.ready && it->second.problem.empty())) return Tr("Вид считается…");
    return it->second.problem;
}

void DrawingViews::Finish(Workspace& ws, DrawingState& d) {
    for (const dw::Sheet& sheet : d.drawing.sheets)
        for (const dw::View& v : dw::ViewsOf(sheet))
            for (int guard = 0; guard < 1000; ++guard) {
                Lines(ws, d, v);
                if (!myJob.valid()) break;
                myJob.wait();
                Poll();
            }
}

std::optional<kernel::Bounds> DrawingViews::ModelBounds(Workspace& ws, DrawingState& d, ObjectId model, std::string* problem) {
    std::string key, why;
    const kernel::Model* m = ModelOf(ws, d, model, key, why);
    if (!m) {
        if (problem) *problem = why;
        return std::nullopt;
    }
    auto box = m->BoundingBox();
    if (!box && problem) *problem = Tr("В модели нет тел");
    return box;
}

} // namespace bz::app
