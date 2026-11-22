// conn.*: «Продолжить на устройстве» for a module (ADR-0011, BreezCONN). The
// module brings the network, the QR and the windows; what goes into the
// package and how the changes come back stays here (assembly/Handoff.h), so
// a module never writes the user's documents itself.

#include "app/AppContext.h"
#include "app/Autosave.h"
#include "app/Operations.h"
#include "app/Session.h"
#include "app/Workspace.h"
#include "app/features/OperationEditors.h"
#include "app/scripting/Commands.h"
#include "assembly/Handoff.h"
#include "core/PathText.h"
#include "core/Version.h"

#include <filesystem>
#include <format>
#include <functional>

namespace bz::app::scripting {

namespace {

namespace fs = std::filesystem;
namespace ho = assembly::handoff;

fs::path AbsolutePath(const Json& args, const char* key) {
    const fs::path p = PathFromUtf8(Text(args, key));
    if (!p.is_absolute()) Fail(std::format("«{}»: нужен полный путь", key));
    return p;
}

bool Same(const fs::path& a, const fs::path& b) { return a.lexically_normal() == b.lexically_normal(); }

/// The document's file as saved: unsaved changes are saved first (the device
/// gets the files as they are on disk).
fs::path SavedDocument(AppContext& app, const Json& args) {
    Session* s = nullptr;
    if (Has(args, "document")) {
        const fs::path file = AbsolutePath(args, "document");
        for (const auto& open : app.workspace.sessions)
            if (!open->path.empty() && Same(open->path, file)) s = open.get();
        if (!s) {
            if (!fs::is_regular_file(file)) Fail(std::format("Нет файла {}", ToUtf8(file)));
            return file;   // not open: as it lies
        }
    } else {
        s = &Active(app);
    }
    if (s->host) s = s->host;   // a part edited inside its assembly goes with the assembly
    app.ops.Commit(app, *s);
    if (s->path.empty()) Fail("Документ ещё ни разу не сохранялся — сначала doc.save с path");
    if (s->IsModified()) {
        std::string error;
        if (!app.workspace.SaveTo(*s, s->path, app.notes, app.settings, &error)) Fail(std::format("Не удалось сохранить: {}", error));
        app.autosave.Discard(*s);
    }
    return s->path;
}

ho::Session LoadedSession(const Json& args) {
    Expected<ho::Session> s = ho::LoadSession(AbsolutePath(args, "session"));
    if (!s) Fail(s.GetError().message);
    return std::move(*s);
}

const char* ChangeName(ho::Change c) {
    switch (c) {
        case ho::Change::Update: return "update";
        case ho::Change::Conflict: return "conflict";
        case ho::Change::Added: return "added";
    }
    return "update";
}

Json Pack(AppContext& app, const Json& args) {
    const fs::path document = SavedDocument(app, args);
    const fs::path package  = AbsolutePath(args, "package");
    fs::path       file     = package;
    file += ".session.json";
    if (Has(args, "session")) file = AbsolutePath(args, "session");
    std::error_code ec;
    if (package.has_parent_path()) fs::create_directories(package.parent_path(), ec);
    if (file.has_parent_path()) fs::create_directories(file.parent_path(), ec);
    Expected<ho::Session> s = ho::Send(document, package, Version());
    if (!s) Fail(s.GetError().message);
    if (Status saved = ho::SaveSession(*s, file); !saved) Fail(saved.GetError().message);
    Json files = Json::array();
    for (const ho::SentFile& f : s->files) files.push_back({{"path", f.path}, {"source", ToUtf8(f.source)}});
    return {{"session", ToUtf8(file)}, {"package", ToUtf8(package)}, {"bytes", fs::file_size(package, ec)},
            {"id", s->id},        {"key", s->key},              {"kind", s->kind},
            {"main", s->main},    {"document", ToUtf8(document)}, {"files", files}};
}

Json Plan(AppContext&, const Json& args) {
    const ho::Session session = LoadedSession(args);
    Expected<std::vector<ho::Returned>> plan = ho::PlanReturn(session, AbsolutePath(args, "returned"));
    if (!plan) Fail(plan.GetError().message);
    Json files = Json::array();
    for (const ho::Returned& r : *plan)
        files.push_back({{"path", r.path}, {"change", ChangeName(r.change)}, {"target", ToUtf8(r.target)}});
    return {{"files", files}};
}

/// Closes the open documents `which` picks (they are saved: the package was
/// made from the files) and gives back their paths.
std::vector<fs::path> CloseWhere(AppContext& app, const std::function<bool(const fs::path&)>& which) {
    std::vector<fs::path> closed;
    for (int i = static_cast<int>(app.workspace.sessions.size()) - 1; i >= 0; --i) {
        const Session& s = *app.workspace.sessions[static_cast<std::size_t>(i)];
        if (s.path.empty() || !which(s.path)) continue;
        if (i == app.workspace.ActiveIndex()) EndEditing(app);
        closed.push_back(s.path);
        app.workspace.Close(i);
    }
    return closed;
}

Json Apply(AppContext& app, const Json& args) {
    const ho::Session session  = LoadedSession(args);
    const fs::path    returned = AbsolutePath(args, "returned");
    const Json&       given    = Required(args, "choices");
    if (!given.is_array()) Fail("«choices»: нужен список, по одному выбору на файл из conn.plan");
    std::vector<ho::Choice> choices;
    for (const Json& c : given) {
        const std::string v = c.is_string() ? c.get<std::string>() : std::string{};
        if (v == "device") choices.push_back(ho::Choice::TakeDevice);
        else if (v == "mine") choices.push_back(ho::Choice::KeepMine);
        else if (v == "both") choices.push_back(ho::Choice::KeepBoth);
        else Fail("«choices»: каждый выбор — \"device\", \"mine\" или \"both\"");
    }
    fs::path backups = AbsolutePath(args, "session").parent_path() / "Копии";
    if (Has(args, "backups")) backups = AbsolutePath(args, "backups");
    const Expected<ho::ApplyReport> r = ho::ApplyReturn(session, returned, choices, backups, Version());
    if (!r) Fail(r.GetError().message);
    // The documents the changes touched are opened again from their files:
    // the main one always (an assembly shows its parts as they are now).
    const fs::path main = session.files.empty() ? fs::path{} : session.files.front().source;
    const std::vector<fs::path> reopen = CloseWhere(app, [&](const fs::path& p) {
        if (!main.empty() && Same(p, main)) return true;
        for (const fs::path& w : r->written)
            if (Same(p, w)) return true;
        return false;
    });
    for (auto it = reopen.rbegin(); it != reopen.rend(); ++it) app.workspace.Open(*it, app.notes, app.settings);
    Json written = Json::array(), saved = Json::array(), opened = Json::array();
    for (const fs::path& p : r->written) written.push_back(ToUtf8(p));
    for (const fs::path& p : r->backups) saved.push_back(ToUtf8(p));
    for (const fs::path& p : reopen) opened.push_back(ToUtf8(p));
    return {{"written", written}, {"backups", saved}, {"reopened", opened}};
}

} // namespace

void AddConnCommands(Commands& out) {
    out.push_back({"conn.pack",
                   "«Продолжить на устройстве»: упаковывает деталь или сборку (со всеми деталями) в package для "
                   "устройства и записывает сессию — что куда вернуть. Несохранённое сохраняется. Ответ: session (файл "
                   "сессии для conn.plan/conn.apply), id и key (для QR-кода и шифрования), kind, main, files.",
                   Schema({{"document", StringArg("Полный путь .bzc/.bza; не задан — активный документ.")},
                           {"package", StringArg("Полный путь пакета для устройства (папка создаётся).")},
                           {"session", StringArg("Полный путь файла сессии; не задан — package + \".session.json\".")}},
                          {"package"}),
                   true, &Pack});
    out.push_back({"conn.plan",
                   "Что прислало устройство и что сделает каждый файл: update (заменит), conflict (изменён и здесь — "
                   "решает пользователь), added (новый документ). Ошибка — если изменения от другого проекта.",
                   Schema({{"session", StringArg("Файл сессии из conn.pack.")},
                           {"returned", StringArg("Полный путь ZIP, присланного устройством.")}},
                          {"session", "returned"}),
                   false, &Plan});
    out.push_back({"conn.apply",
                   "Кладёт изменения с устройства на место, по выбору на каждый файл из conn.plan; затронутые открытые "
                   "документы открываются заново. Ответ: written, backups (прежние файлы), reopened.",
                   Schema({{"session", StringArg("Файл сессии из conn.pack.")},
                           {"returned", StringArg("Полный путь ZIP, присланного устройством.")},
                           {"choices", {{"type", "array"},
                                        {"description", "По одному на файл из conn.plan, в том же порядке: device — файл "
                                                        "с устройства, mine — оставить свой, both — оба (с устройства "
                                                        "ляжет рядом)."},
                                        {"items", {{"type", "string"}, {"enum", {"device", "mine", "both"}}}}}},
                           {"backups", StringArg("Куда сложить заменённые файлы; не задано — «Копии» рядом с файлом сессии.")}},
                          {"session", "returned", "choices"}),
                   true, &Apply});
}

} // namespace bz::app::scripting
