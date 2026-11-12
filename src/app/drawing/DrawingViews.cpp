#include "app/drawing/DrawingViews.h"

#include "app/AssemblyState.h"
#include "app/Materials.h"
#include "app/Session.h"
#include "app/Workspace.h"
#include "app/drawing/DrawingState.h"
#include "core/I18n.h"
#include "core/PathText.h"
#include "kernel/AssemblyReadApi.h"

#include <chrono>
#include <cmath>
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

Session* DrawingViews::SessionOf(Workspace& ws, DrawingState& d, ObjectId id, std::string& sourceKey, std::string& problem) {
    const std::filesystem::path file = ModelFile(d, id);
    if (file.empty()) {
        problem = Tr("Модели этого вида нет в чертеже");
        return nullptr;
    }
    // Open in a tab: as it is now, edits included (a part or an assembly).
    for (const auto& s : ws.sessions)
        if (!s->drawing && !s->path.empty() && SameFile(s->path, file)) {
            sourceKey = std::format("tab:{}:{}", ToUtf8(file), s->model.Revision());
            return s.get();
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
            } else if ((*loaded)->drawing) {
                src->session.reset();
                src->problem = Tr("Вид чертежа из другого чертежа не строится");
            } else {
                src->session = std::move(*loaded);
                src->session->path = file;   // an assembly finds its parts beside it
                src->session->RebuildNow();
                src->problem.clear();
            }
        } else if (src->session->assembly) {
            src->session->RebuildNow();   // its parts read again if they changed on disk
        }
    }
    if (!src->session) {
        problem = src->problem;
        return nullptr;
    }
    sourceKey = std::format("file:{}:{}:{}", name, src->written.time_since_epoch().count(), src->session->model.Revision());
    return src->session.get();
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
    Session* session = SessionOf(ws, d, view.model, sourceKey, problem);
    const kernel::Model* model = session ? &session->model : nullptr;
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
    Session* session = SessionOf(ws, d, model, key, why);
    const kernel::Model* m = session ? &session->model : nullptr;
    if (!m) {
        if (problem) *problem = why;
        return std::nullopt;
    }
    auto box = m->BoundingBox();
    if (!box && problem) *problem = Tr("В модели нет тел");
    return box;
}

namespace {

/// Mass in kilograms as the title block writes it: «0,125», «1,25», «12,5», «125».
std::string MassText(double kg) {
    if (kg <= 0) return {};
    const int decimals = kg >= 100 ? 0 : kg >= 10 ? 1 : kg >= 1 ? 2 : 3;
    std::string s = std::format("{:.{}f}", kg, decimals);
    if (s.find('.') != std::string::npos) {
        while (s.back() == '0') s.pop_back();
        if (s.back() == '.') s.pop_back();
    }
    for (char& c : s)
        if (c == '.') c = ',';
    return s;
}

/// kg of `volume` mm³ of `material`; 0 when its density is not known.
double Mass(double volume, const std::string& material) {
    const Material* m = FindMaterial(material);
    return m ? volume * m->density * 1e-6 : 0.0;
}

std::string SizeText(dw::SheetSize size) { return std::format("А{}", std::string(dw::SizeId(size)).substr(1)); }

} // namespace

ObjectId DrawingViews::ModelOfSheet(const DrawingState& d, const dw::Sheet& sheet) {
    if (sheet.kind == dw::SheetKind::Specification) return sheet.model;
    for (const dw::View& v : dw::ViewsOf(sheet)) return v.model;
    return d.drawing.models.empty() ? ObjectId{} : d.drawing.models.front().id;
}

dw::Fields DrawingViews::AutoFields(Workspace& ws, DrawingState& d, const dw::Sheet& sheet) {
    dw::Fields out;
    // The scale of the sheet: of its first (main) view.
    if (sheet.kind == dw::SheetKind::Drawing)
        for (const dw::View& v : dw::ViewsOf(sheet)) {
            out[dw::field::kScale] = dw::ScaleText(v.scale);
            break;
        }
    const ObjectId model = ModelOfSheet(d, sheet);
    if (!model) return out;
    std::string key, problem;
    Session* s = SessionOf(ws, d, model, key, problem);
    if (!s) return out;
    Properties& p = myProperties[model];
    if (p.key != key) {
        p = {};
        p.key = key;
        const std::string stem = ToUtf8(ModelFile(d, model).stem());
        if (s->assembly) {
            p.assembly = true;
            const assembly::Assembly& a = s->assembly->assembly;
            p.fields[dw::field::kName] = a.info.name.empty() ? stem : a.info.name;
            p.fields[dw::field::kDesignation] = a.info.designation;
            // The mass of an assembly: of its components, each of its own material.
            const kernel::AssemblyReader reader(a, s->assembly->geometry);
            double kg = 0;
            bool known = true;
            for (const kernel::ComponentRecord& c : reader.Components()) {
                const auto bodies = reader.BodiesOf(c.id);
                if (!bodies) continue;
                const double m = Mass(bodies->Volume(), c.material) * std::max(1, c.quantity);
                if (m <= 0) known = false;
                kg += m;
            }
            if (known) p.fields[dw::field::kMass] = MassText(kg);
        } else {
            const doc::PartInfo& info = s->doc.info;
            p.fields[dw::field::kName] = info.name.empty() ? stem : info.name;
            p.fields[dw::field::kDesignation] = info.designation;
            p.fields[dw::field::kMaterial] = info.material;
            p.fields[dw::field::kMass] = MassText(Mass(s->model.Volume(), info.material));
        }
    }
    for (const auto& [k, v] : p.fields) out[k] = v;
    // ГОСТ 2.102: the assembly drawing is «… СБ»; its specification is the assembly itself.
    if (p.assembly && sheet.kind == dw::SheetKind::Drawing && !out[dw::field::kDesignation].empty())
        out[dw::field::kDesignation] += " СБ";
    return out;
}

std::vector<dw::SpecRow> DrawingViews::SpecRows(Workspace& ws, DrawingState& d, const dw::Sheet& sheet) {
    if (sheet.kind != dw::SheetKind::Specification || !sheet.model) return {};
    std::string key, problem;
    Session* s = SessionOf(ws, d, sheet.model, key, problem);
    if (!s || !s->assembly) return {};
    // The assembly drawing in this file: the format of its first sheet with views of the assembly.
    std::string format;
    for (const dw::Sheet& other : d.drawing.sheets)
        if (other.kind == dw::SheetKind::Drawing)
            for (const dw::View& v : dw::ViewsOf(other))
                if (v.model == sheet.model && format.empty()) format = SizeText(other.size);
    Properties& p = myProperties[sheet.model];
    const std::string rowsKey = key + "|" + format;
    if (p.rowsKey == rowsKey) return p.rows;
    const assembly::Assembly& a = s->assembly->assembly;
    dw::SpecInput in;
    in.designation = a.info.designation;
    in.drawingFormat = format;
    const kernel::AssemblyReader reader(a, s->assembly->geometry);
    for (const kernel::ComponentRecord& r : reader.Components()) {
        dw::SpecComponent c;
        c.path = r.path;
        c.designation = r.designation;
        c.name = r.name;
        c.quantity = r.quantity;
        for (const assembly::Component& comp : a.components)
            if (comp.id == r.id) {
                c.position = comp.properties.position;
                c.note = comp.properties.note;
            }
        in.components.push_back(std::move(c));
    }
    p.rows = dw::SpecRows(in);
    p.rowsKey = rowsKey;
    return p.rows;
}

dw::SheetContent DrawingViews::Content(Workspace& ws, DrawingState& d) {
    dw::SheetContent c;
    c.lines = [this, &ws, &d](const dw::Sheet&, const dw::View& v) { return Lines(ws, d, v); };
    c.fields = [this, &ws, &d](const dw::Sheet& s) { return AutoFields(ws, d, s); };
    c.spec = [this, &ws, &d](const dw::Sheet& s) { return SpecRows(ws, d, s); };
    return c;
}

} // namespace bz::app
