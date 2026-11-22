#include "app/Import.h"

#include "app/Format.h"
#include "app/Operations.h"
#include "app/Session.h"
#include "app/Workspace.h"
#include "app/shell/Shell.h"
#include "core/I18n.h"
#include "core/Log.h"
#include "core/PathText.h"
#include "document/Commands.h"
#include "document/ImportFeature.h"
#include "kernel/Import.h"
#include "platform/FileDialogs.h"
#include "platform/Paths.h"
#include "platform/Window.h"
#include "ui/ViewCube.h"
#include "ui/Widgets.h"

#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <format>
#include <memory>
#include <thread>

namespace bz::app {

namespace {

/// What the thread and the window share. The thread keeps it alive by
/// itself: a closed tab drops its ImportJob, the thread finishes (cancelled)
/// into a state nobody reads.
struct Shared {
    std::filesystem::path          file;
    kernel::ImportOptions          options;
    kernel::ImportProgress         progress;
    std::atomic<bool>              done{false};
    Expected<kernel::ImportResult> result = Error{};   ///< written by the thread before `done`
};

std::uint64_t TriangleLimit(const AppContext& app) {
#if defined(BZ_MOBILE)
    (void)app;
    return kernel::kMeshTriangleLimitPhone;
#else
    return app.shell.mobile.phone ? kernel::kMeshTriangleLimitPhone : kernel::kMeshTriangleLimitDesktop;
#endif
}

std::string Count(std::uint64_t n) {
    // 2 000 000: thin spaces between thousands read as one number in any language.
    std::string digits = std::to_string(n), out;
    for (std::size_t i = 0; i < digits.size(); ++i) {
        if (i > 0 && (digits.size() - i) % 3 == 0) out += "\xC2\xA0";
        out += digits[i];
    }
    return out;
}

std::string Megabytes(std::uintmax_t bytes) {
    const double mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
    return mb < 10 ? std::format("{:.1f}", mb) : std::format("{:.0f}", mb);
}

std::string FileName(const std::filesystem::path& file) { return ToUtf8(file.filename()); }

/// "1,8 МБ", "640 КБ": the size of what is kept inside the part.
std::string Size(std::uintmax_t bytes) {
    if (bytes < 1024 * 1024) return Trf("{} КБ", std::max<std::uintmax_t>(1, (bytes + 512) / 1024));
    return Trf("{} МБ", Megabytes(bytes));
}

/// The length unit a STEP file names (OCCT spells them "INCH", "MM"...), as
/// people say it; empty for millimetres (nothing to tell).
std::string UnitName(std::string unit) {
    for (char& ch : unit) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));   // OCCT says "INCH" or "millimetre"
    if (unit.empty() || unit == "MM" || unit == "MILLIMETRE") return {};
    if (unit == "INCH" || unit == "IN") return Tr("дюймы");
    if (unit == "M" || unit == "METRE") return Tr("метры");
    if (unit == "CM" || unit == "CENTIMETRE") return Tr("сантиметры");
    if (unit == "FT" || unit == "FOOT") return Tr("футы");
    if (unit == "UM" || unit == "MICROMETRE") return Tr("микрометры");
    if (unit == "MIL") return Tr("тысячные дюйма");
    if (unit == "KM" || unit == "KILOMETRE") return Tr("километры");
    return unit;
}

void Paragraph(ui::Ui& ui, const std::string& text, const Color& color, ImFont* font = nullptr) {
    ui::FontScope f(font ? font : ui.fonts.body);
    ImGui::PushStyleColor(ImGuiCol_Text, ui::ToVec4(color));
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

} // namespace

struct ImportJob {
    std::shared_ptr<Shared> shared = std::make_shared<Shared>();
    ObjectId    updateOf;        ///< «Обновить из файла»: the feature to replace; none for a new part
    bool        failed = false;  ///< finished with an error: the card says it
    std::string error;
    bool        taken = false;   ///< the result went into the part
    ~ImportJob() { shared->progress.cancel = true; }   // the tab went: the thread may stop
};

struct ImportState {
    bool                  open = false;
    std::filesystem::path file;
    kernel::ImportProbe   probe;
    bool                  dense = false;   ///< a mesh over the limit: simplify or keep
    bool                  large = false;
    bool                  simplify = true;
    std::uint64_t         limit = 0;
    std::string           limitText;
    Session*              session = nullptr;   ///< «Обновить из файла» of this session's feature
    ObjectId              updateOf;
};

namespace {

ImportState& State(AppContext& app) {
    if (!app.shell.import) app.shell.import = std::make_shared<ImportState>();
    return *app.shell.import;
}

/// Starts reading `file` on another thread for `session`.
void Launch(Session& session, const std::filesystem::path& file, const kernel::ImportOptions& options, ObjectId updateOf) {
    auto job = std::make_shared<ImportJob>();
    job->updateOf = updateOf;
    job->shared->file = file;
    job->shared->options = options;
    session.importing = job;
    std::thread([shared = job->shared] {
        shared->result = kernel::ImportFile(shared->file, shared->options, &shared->progress);
        shared->done = true;
        platform::Window::PostEmptyEvent();   // the window sleeps until something happens
    }).detach();
    log::Info("Import of {} started", ToUtf8(file));
}

/// A new tab for `file`, named after it.
Session& NewImportTab(AppContext& app, const std::filesystem::path& file) {
    NewDocument(app);   // ends any editing first
    Session& s = *app.workspace.Active();
    s.untitledName = ToUtf8(file.stem());
    return s;
}

/// A tab that only shows why `file` could not be imported.
void FailedTab(AppContext& app, const std::filesystem::path& file, std::string message) {
    Session& s = NewImportTab(app, file);
    auto job = std::make_shared<ImportJob>();
    job->shared->file = file;
    job->failed = true;
    job->taken = true;
    job->error = std::move(message);
    s.importing = job;
}

/// Asks first when there is something to decide; otherwise starts.
void Begin(AppContext& app, const std::filesystem::path& file, Session* target, ObjectId updateOf) {
    const Expected<kernel::ImportProbe> probe = kernel::ProbeImport(file);
    if (!probe) {
        if (target) app.notes.Error(Trf("Не удалось обновить из файла: {}", Tr(probe.GetError().message)));
        else FailedTab(app, file, probe.GetError().message);
        return;
    }
    const std::uint64_t limit = TriangleLimit(app);
    ImportState& st = State(app);
    st = {};
    st.file = file;
    st.probe = *probe;
    st.dense = probe->format == kernel::ImportFormat::Stl && probe->triangles > limit;
    st.large = kernel::IsLargeImport(*probe);
    st.limit = limit;
    st.limitText = Count(limit);
    st.session = target;
    st.updateOf = updateOf;
    if (st.dense || st.large) {
        st.open = true;
        return;
    }
    Session& s = target ? *target : NewImportTab(app, file);
    Launch(s, file, {}, updateOf);
}

void Proceed(AppContext& app) {
    ImportState& st = State(app);
    kernel::ImportOptions options;
    if (st.dense && st.simplify) options.maxTriangles = std::max<std::uint64_t>(st.limit, 1000);
    Session* target = st.session && app.workspace.IsOpen(st.session) ? st.session : nullptr;
    if (st.session && !target) return;   // the part to update was closed meanwhile
    Session& s = target ? *target : NewImportTab(app, st.file);
    Launch(s, st.file, options, st.updateOf);
}

const char* StageName(kernel::ImportProgress::Stage stage, bool mesh) {
    using Stage = kernel::ImportProgress::Stage;
    switch (stage) {
        case Stage::Reading:     return Tr("Чтение файла");
        case Stage::Translating: return mesh ? Tr("Разбор сетки") : Tr("Перевод геометрии");
        case Stage::Healing:     return Tr("Лечение геометрии");
        case Stage::Simplifying: return Tr("Упрощение сетки");
        case Stage::Packing:     return Tr("Упаковка");
    }
    return "";
}

/// Puts a finished import into its part.
void Take(AppContext& app, Session& s, ImportJob& job) {
    job.taken = true;
    Shared& sh = *job.shared;
    if (!sh.result) {
        const std::string& message = sh.result.GetError().message;
        if (message == kernel::kImportCancelled) {
            if (!job.updateOf.IsValid()) {   // a new part that never was: its tab goes
                for (std::size_t i = 0; i < app.workspace.sessions.size(); ++i)
                    if (app.workspace.sessions[i].get() == &s) {
                        Session* gone = &s;
                        app.Later([&app, gone] {
                            for (std::size_t k = 0; k < app.workspace.sessions.size(); ++k)
                                if (app.workspace.sessions[k].get() == gone) app.workspace.Close(static_cast<int>(k));
                        });
                        break;
                    }
            } else {
                s.importing.reset();
            }
            app.notes.Info(Tr("Импорт отменён"));
            return;
        }
        log::Warn("Import of {}: {}", ToUtf8(sh.file), message);
        if (job.updateOf.IsValid()) {
            app.notes.Error(Trf("Не удалось обновить из файла: {}", Tr(message)));
            s.importing.reset();
            return;
        }
        job.failed = true;
        job.error = message;
        return;
    }

    kernel::ImportResult& r = *sh.result;
    // Everything said afterwards is taken now: the feature moves into the
    // document, and `job` itself goes when the import is done.
    const bool          mesh = r.feature.kind == doc::ImportKind::Mesh;
    const std::string   unit = r.feature.sourceUnit;
    const std::uint64_t triangles = r.feature.triangles;
    const int           bodies = r.feature.bodyCount;
    const double        seconds = r.seconds;
    const std::vector<std::string> warnings = std::move(r.warnings);
    if (job.updateOf.IsValid()) {
        const auto* old = s.doc.FindAs<doc::ImportFeature>(job.updateOf);
        if (!old) {
            s.importing.reset();
            return;
        }
        const std::string name = old->name;   // `old` is replaced below
        auto edited = std::make_unique<doc::ImportFeature>(std::move(r.feature));
        edited->id = old->id;
        edited->name = old->name;
        edited->suppressed = old->suppressed;
        edited->hidden = old->hidden;
        s.Execute(doc::ReplaceFeature(std::move(edited), Tr("Обновить из файла")));
        app.notes.Success(Trf("«{}» обновлено из файла", name));
    } else {
        auto f = std::make_unique<doc::ImportFeature>(std::move(r.feature));
        f->id = s.doc.Ids().Next();
        f->name = s.doc.UniqueFeatureName(Tr("Импортированное тело"));
        s.Execute(doc::AddFeature(std::move(f), 0));
        app.notes.Success(mesh ? Trf("Импорт завершён: сетка, {} треугольников · {:.1f} с", Count(triangles), seconds)
                               : Trf("Импорт завершён: тел — {} · {:.1f} с", bodies, seconds));
    }
    // What the file needed: units, healing, simplifying — said once, in the log too.
    if (const std::string name = UnitName(unit); !name.empty()) app.notes.Info(Trf("Единицы файла — {}, переведены в мм", name));
    for (const std::string& w : warnings) {
        log::Info("Import: {}", w);
        app.notes.Info(Tr(w));
    }
    const bool update = job.updateOf.IsValid();
    s.importing.reset();   // `job` is gone from here on
    s.RebuildNow();
    if (update) return;    // the camera stays where the user left it
    // A new part: the whole model in view, from the home direction. The
    // viewport has not shown the new bodies yet, so the box comes from the model.
    Box3 box;
    if (const auto bounds = s.model.BoundingBox()) {
        box.Include(bounds->min);
        box.Include(bounds->max);
    }
    if (app.Doc() != &s) {   // another tab on screen: this one fits when shown
        s.hasCamera = false;
        return;
    }
    s.camera = app.viewport.FitCamera(-ui::kHomeDirection, {0, 0, 1}, box);
    s.hasCamera = true;
    app.viewport.SetCamera(s.camera);
}

} // namespace

bool IsImportable(const std::filesystem::path& file) {
    std::u8string ext = file.extension().u8string();
    for (char8_t& ch : ext)
        if (ch >= u8'A' && ch <= u8'Z') ch = static_cast<char8_t>(ch - u8'A' + u8'a');
    return ext == u8".step" || ext == u8".stp" || ext == u8".stl";
}

void StartImport(AppContext& app, std::optional<std::filesystem::path> file) {
    if (!file) {
        if (const std::string problem = platform::FileDialogProblem(); !problem.empty()) {
            app.notes.Error(problem);
            return;
        }
        const std::filesystem::path folder =
            !app.settings.recentFiles.empty() ? app.settings.recentFiles.front().parent_path() : platform::DocumentsDir();
        file = platform::ShowOpenDialog(Tr("Импорт"),
                                        {{Tr("STEP и STL"), "*.step *.stp *.stl *.STEP *.STP *.STL"},
                                         {Tr("STEP — точная деталь"), "*.step *.stp *.STEP *.STP"},
                                         {Tr("STL — сетка"), "*.stl *.STL"}},
                                        folder);
        if (!file) return;
    }
    Begin(app, *file, nullptr, {});
}

void UpdateFromFile(AppContext& app, Session& session, ObjectId id) {
    const auto* f = session.doc.FindAs<doc::ImportFeature>(id);
    if (!f || session.importing) return;
    std::filesystem::path file = PathFromUtf8(f->source);
    std::error_code ec;
    if (f->source.empty() || !std::filesystem::exists(file, ec)) {
        // Moved, renamed, or the part came from another computer: ask where it is now.
        app.notes.Info(Trf("Исходный файл не найден: {} — укажите, где он теперь", f->source));
        if (const std::string problem = platform::FileDialogProblem(); !problem.empty()) {
            app.notes.Error(problem);
            return;
        }
        const std::optional<std::filesystem::path> picked = platform::ShowOpenDialog(
            Tr("Обновить из файла"), {{Tr("STEP и STL"), "*.step *.stp *.stl *.STEP *.STP *.STL"}}, file.parent_path());
        if (!picked) return;
        file = *picked;
    }
    Begin(app, file, &session, id);
}

void TickImports(AppContext& app) {
    for (const auto& s : app.workspace.sessions) {
        if (!s->importing) continue;
        ImportJob& job = *s->importing;
        if (!job.taken && job.shared->done) Take(app, *s, job);
    }
}

bool ImportsRunning(const AppContext& app) {
    for (const auto& s : app.workspace.sessions)
        if (s->importing && !s->importing->taken) return true;
    return false;
}

std::optional<double> ImportProgressOf(const Session& session) {
    if (!session.importing || session.importing->taken) return std::nullopt;
    return std::clamp(session.importing->shared->progress.fraction.load(), 0.0, 1.0);
}

void CancelImport(Session& session) {
    if (session.importing) session.importing->shared->progress.cancel = true;
}

void DrawImportCard(AppContext& app, Session& session, ImVec2 viewMin, ImVec2 viewMax) {
    if (!session.importing) return;
    ImportJob& job = *session.importing;
    Shared& sh = *job.shared;
    ui::Ui& ui = app.ui;
    const ui::ThemeColors& c = ui.C();
    const float width = std::min(ui.S(job.failed ? 470.0f : 430.0f), viewMax.x - viewMin.x - ui.S(24));
    ImGui::SetNextWindowPos({(viewMin.x + viewMax.x) / 2, viewMin.y + (viewMax.y - viewMin.y) * 0.44f}, ImGuiCond_Always, {0.5f, 0.5f});
    ImGui::SetNextWindowSize({width, 0});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ui.S(22, 20));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, ui.S(10));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ui.S(8, 10));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ui::ToVec4(c.panel));
    ImGui::PushStyleColor(ImGuiCol_Border, ui::ToVec4(job.failed ? c.danger.WithAlpha(0.45f) : c.divider));
    const bool shown = ImGui::Begin("##import-card", nullptr,
                                    ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(4);
    if (!shown) {
        ImGui::End();
        return;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float inner = ImGui::GetContentRegionAvail().x;
    const auto heading = [&](const char* icon, const Color& tint, const std::string& text) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ui.icons.Draw(dl, icon, p, ui.S(20), tint);
        ImGui::SetCursorScreenPos({p.x + ui.S(30), p.y + ui.S(1)});
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + inner - ui.S(30));
        {
            ui::FontScope f(ui.fonts.title);
            ImGui::PushStyleColor(ImGuiCol_Text, ui::ToVec4(c.text));
            ImGui::TextUnformatted(text.c_str());
            ImGui::PopStyleColor();
        }
        ImGui::PopTextWrapPos();
    };

    if (job.failed) {
        heading("ui-close", c.danger, Trf("Не удалось импортировать «{}»", FileName(sh.file)));
        Paragraph(ui, Tr(job.error), c.text);
        Paragraph(ui,
                  Tr("Попробуйте пересохранить файл в исходной программе (STEP AP214 или AP242, STL). Программа и другие документы в порядке."),
                  c.textSecondary, ui.fonts.bodySmall);
        {
            ui::FontScope f(ui.fonts.mono);
            const std::string where = ToUtf8(sh.file);
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float h = ui.S(28);
            dl->AddRectFilled(p, {p.x + inner, p.y + h}, ui::ToU32(c.appBackground), ui.S(6));
            ImGui::SetCursorScreenPos({p.x + ui.S(10), p.y + (h - ImGui::GetFontSize()) / 2});
            ImGui::PushStyleColor(ImGuiCol_Text, ui::ToVec4(c.textMuted));
            ui::ClippedText(where.c_str(), inner - ui.S(20));
            ImGui::PopStyleColor();
            ImGui::SetCursorScreenPos({p.x, p.y + h + ui.S(4)});
        }
        const float bw = (inner - ui.S(8)) / 2;
        if (ui::SecondaryButton(ui, "##import-copy", Tr("Копировать сведения"), nullptr, {bw, ui.S(36)})) {
            const std::string text = std::format("BreezeCAD: {}\n{}\n{}", Tr("Импорт не выполнен"), Tr(job.error), ToUtf8(sh.file));
            ImGui::SetClipboardText(text.c_str());
            app.notes.Success(Tr("Сведения скопированы"));
        }
        ImGui::SameLine(0, ui.S(8));
        if (ui::PrimaryButton(ui, "##import-close", Tr("Закрыть вкладку"), nullptr, {bw, ui.S(36)})) {
            Session* gone = &session;
            app.Later([&app, gone] {
                for (std::size_t k = 0; k < app.workspace.sessions.size(); ++k)
                    if (app.workspace.sessions[k].get() == gone) app.workspace.Close(static_cast<int>(k));
            });
        }
        ImGui::End();
        return;
    }

    // Under way.
    const bool mesh = !sh.file.empty() && FileName(sh.file).ends_with(".stl");
    heading("ui-import", c.accent,
            job.updateOf.IsValid() ? Trf("Обновление из «{}»", FileName(sh.file)) : Trf("Импорт «{}»", FileName(sh.file)));
    using Stage = kernel::ImportProgress::Stage;
    const Stage stage = sh.progress.stage.load();
    const double fraction = std::clamp(sh.progress.fraction.load(), 0.0, 1.0);
    {
        ui::FontScope f(ui.fonts.bodySmall);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const std::string percent = std::format("{}%", static_cast<int>(std::lround(fraction * 100)));
        dl->AddText(p, ui::ToU32(c.text), StageName(stage, mesh));
        const float pw = ImGui::CalcTextSize(percent.c_str()).x;
        dl->AddText(ui.fonts.mono, ImGui::GetFontSize(), {p.x + inner - pw, p.y}, ui::ToU32(c.text), percent.c_str());
        const float barY = p.y + ImGui::GetFontSize() + ui.S(8), barH = ui.S(6);
        dl->AddRectFilled({p.x, barY}, {p.x + inner, barY + barH}, ui::ToU32(c.dividerSoft), barH / 2);
        dl->AddRectFilled({p.x, barY}, {p.x + std::max(barH, inner * static_cast<float>(fraction)), barY + barH}, ui::ToU32(c.accent),
                          barH / 2);
        // The stages: done ones ticked, the current one dark.
        float x = p.x;
        const float sy = barY + barH + ui.S(8);
        std::vector<Stage> stages{Stage::Reading, Stage::Translating};
        if (!mesh) stages.push_back(Stage::Healing);
        if (mesh && sh.options.maxTriangles > 0) stages.push_back(Stage::Simplifying);
        stages.push_back(Stage::Packing);
        for (Stage st : stages) {
            const bool done = static_cast<int>(st) < static_cast<int>(stage);
            const bool now = st == stage;
            // Done: a tick icon (the interface font has no ✓); now: a bullet.
            const std::string label = (now ? std::string("\xE2\x80\xA2 ") : std::string()) + StageName(st, mesh);
            ui::FontScope small(ui.fonts.caption);
            if (done) {
                const float icon = ImGui::GetFontSize();
                ui.icons.Draw(dl, "ui-check", {x, sy}, icon, c.success);
                x += icon + ui.S(3);
            }
            dl->AddText({x, sy}, ui::ToU32(done ? c.success : now ? c.text : c.textMuted), label.c_str());
            x += ImGui::CalcTextSize(label.c_str()).x + ui.S(14);
        }
        ImGui::Dummy({inner, sy + ui.S(16) - p.y});
    }
    Paragraph(ui, Tr("Можно переключиться на другие вкладки и работать — импорт идёт в фоне."), c.textSecondary, ui.fonts.bodySmall);
    const float bw = ui.S(170);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + inner - bw);
    const bool cancelling = sh.progress.cancel.load();
    if (ui::SecondaryButton(ui, "##import-cancel", cancelling ? Tr("Отменяется…") : Tr("Отменить импорт"), "Esc", {bw, ui.S(36)},
                            !cancelling))
        sh.progress.cancel = true;
    ImGui::End();
}

void DrawImportDialog(AppContext& app) {
    if (!app.shell.import) return;
    ImportState& st = *app.shell.import;
    ui::Ui& ui = app.ui;
    const ui::ThemeColors& c = ui.C();
    if (!BeginDialog(ui, "##import-ask", st.open, ui.S(st.dense ? 520.0f : 480.0f))) return;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float inner = ImGui::GetContentRegionAvail().x;
    const auto close = [&] {
        st.open = false;
        ImGui::CloseCurrentPopup();
    };
    // Heading: icon, title, the file.
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ui.icons.Draw(dl, st.dense ? "tree-mesh" : "tree-import", p, ui.S(28), {1, 1, 1, 1});
        dl->AddText(ui.fonts.title, ui.S(16), {p.x + ui.S(40), p.y}, ui::ToU32(c.text),
                    st.dense ? Tr("Импорт сетки") : Tr("Большой файл"));
        std::string about = FileName(st.file) + " \xC2\xB7 " + Megabytes(st.probe.bytes) + " " + Tr("МБ");
        if (st.dense) about += " \xC2\xB7 " + std::string(st.probe.binary ? Tr("двоичный") : Tr("текстовый"));
        dl->AddText(ui.fonts.mono, ui.S(12), {p.x + ui.S(40), p.y + ui.S(22)}, ui::ToU32(c.textSecondary), about.c_str());
        ImGui::Dummy({inner, ui.S(40)});
    }
    if (st.dense) {
        // The warning, on a soft orange ground.
        const std::string text = Trf("В сетке {} треугольников. Для этого устройства рекомендуется не больше {} — иначе вид будет тормозить.",
                                     Count(st.probe.triangles), st.limitText);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ui::ToVec4(c.warning.WithAlpha(0.08f)));
        ImGui::PushStyleColor(ImGuiCol_Border, ui::ToVec4(c.warning.WithAlpha(0.35f)));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, ui.S(8));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ui.S(12, 10));
        ImGui::BeginChild("##dense-warning", {inner, 0}, ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
        Paragraph(ui, text, c.text, ui.fonts.bodySmall);
        ImGui::EndChild();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(2);

        // Two choices as cards, the chosen one tinted.
        const auto choice = [&](const char* id, bool on, const std::string& title, const std::string& sub) {
            const ImVec2 p = ImGui::GetCursorScreenPos();
            ui::FontScope f(ui.fonts.bodySmall);
            const float textW = inner - ui.S(44);
            const float subH = ImGui::CalcTextSize(sub.c_str(), nullptr, false, textW).y;
            const float h = ui.S(20) + ui.S(18) + subH;
            const bool pressed = ImGui::InvisibleButton(id, {inner, h});
            const bool hovered = ImGui::IsItemHovered();
            dl->AddRectFilled(p, {p.x + inner, p.y + h}, ui::ToU32(on ? c.accentSoft : hovered ? c.hoverRow : c.panel), ui.S(8));
            dl->AddRect(p, {p.x + inner, p.y + h}, ui::ToU32(on ? c.accentSoftBorder : c.divider), ui.S(8));
            const ImVec2 dot{p.x + ui.S(20), p.y + ui.S(19)};
            dl->AddCircle(dot, ui.S(7), ui::ToU32(on ? c.accent : c.textDisabled), 0, ui.S(1.5f));
            if (on) dl->AddCircleFilled(dot, ui.S(3.5f), ui::ToU32(c.accent));
            dl->AddText(ui.fonts.strong, ui.S(13), {p.x + ui.S(36), p.y + ui.S(11)}, ui::ToU32(c.text), title.c_str());
            dl->AddText(ui.fonts.bodySmall, ImGui::GetFontSize(), {p.x + ui.S(36), p.y + ui.S(31)}, ui::ToU32(c.textSecondary),
                        sub.c_str(), nullptr, textW);
            return pressed;
        };
        if (choice("##simplify", st.simplify, Trf("Упростить до {} треугольников", st.limitText),
                   Tr("Мелкие детали сгладятся равномерно по всей модели. Исходный файл не меняется.")))
            st.simplify = true;
        if (choice("##keep", !st.simplify, Tr("Оставить как есть"),
                   Trf("Все {} треугольников. На слабом ноутбуке и телефоне вид может тормозить.", Count(st.probe.triangles))))
            st.simplify = false;
        if (st.large)
            Paragraph(ui, Tr("Файл большой: импорт может занять несколько минут. Его можно отменить в любой момент."), c.textSecondary,
                      ui.fonts.bodySmall);
    } else {
        const double gb = std::max(0.5, static_cast<double>(st.probe.bytes) / (1024.0 * 1024.0 * 1024.0) * 10.0);
        Paragraph(ui,
                  Trf("Импорт может занять несколько минут и потребовать около {:.1f} ГБ памяти. Его можно отменить в любой момент — "
                      "программа и открытые документы не пострадают.",
                      gb),
                  c.text);
        if (st.probe.format == kernel::ImportFormat::Step)
            Paragraph(ui, Tr("Сборки из STEP пока импортируются одной деталью: каждое тело — отдельным телом."), c.textSecondary,
                      ui.fonts.bodySmall);
    }
    ImGui::Dummy({0, ui.S(4)});
    const float bw = (inner - ui.S(8)) / 2;
    if (ui::SecondaryButton(ui, "##import-no", Tr("Отмена"), "Esc", {bw, ui.S(36)}) || ImGui::IsKeyPressed(ImGuiKey_Escape)) close();
    ImGui::SameLine(0, ui.S(8));
    if (ui::PrimaryButton(ui, "##import-go", st.dense ? Tr("Импортировать") : Tr("Продолжить"), nullptr, {bw, ui.S(36)}) ||
        ui::EnterPressed(false)) {
        close();
        app.Later([&app] { Proceed(app); });
    }
    ImGui::EndPopup();
}

void DrawImportProperties(AppContext& app, Session& session, ObjectId id, float width) {
    const auto* f = session.doc.FindAs<doc::ImportFeature>(id);
    if (!f) return;
    ui::Ui& ui = app.ui;
    const ui::ThemeColors& c = ui.C();
    ImGui::Dummy({0, ui.S(10)});
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddLine({p.x, p.y}, {p.x + width, p.y}, ui::ToU32(c.divider));
    }
    const float inner = width - ui.S(28);
    ImGui::SetCursorPos({ui.S(14), ImGui::GetCursorPosY() + ui.S(12)});
    ImGui::BeginGroup();
    {
        ui::FontScope fs(ui.fonts.strong);
        ImGui::TextUnformatted(f->name.c_str());
    }
    const auto row = [&](const char* label, const std::string& value, bool mono = false) {
        ui::FontScope fs(ui.fonts.bodySmall);
        ImGui::PushStyleColor(ImGuiCol_Text, ui::ToVec4(c.textMuted));
        ImGui::TextUnformatted(label);
        ImGui::PopStyleColor();
        ImGui::SameLine(ui.S(110));
        ui::FontScope vf(mono ? ui.fonts.mono : ui.fonts.bodySmall);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + inner - ui.S(110));
        ImGui::TextUnformatted(value.c_str());
        ImGui::PopTextWrapPos();
    };
    row(Tr("Исходный файл"), f->source.empty() ? std::string("—") : ToUtf8(PathFromUtf8(f->source).filename()), true);
    if (ImGui::IsItemHovered() && !f->source.empty()) ui::ItemTooltip(ui, f->source.c_str());
    std::string format = f->sourceFormat;
    if (const std::string name = UnitName(f->sourceUnit); !name.empty()) format += Trf(", единицы файла — {} → мм", name);
    row(Tr("Формат"), format);
    if (f->kind == doc::ImportKind::Mesh) {
        std::string tris = Count(f->triangles);
        if (f->originalTriangles > f->triangles) tris += Trf(" (было {})", Count(f->originalTriangles));
        row(Tr("Треугольников"), tris);
    } else {
        row(Tr("Тела"), std::to_string(f->bodyCount));
    }
    const std::size_t bytes = f->data ? f->data->size() : 0;
    row(Tr("Хранится"), Trf("внутри детали ({})", Size(bytes)));
    ImGui::Dummy({0, ui.S(4)});
    const bool busy = session.importing != nullptr;
    if (ui::SecondaryButton(ui, "##import-update", Tr("Обновить из файла"), nullptr, {inner, ui.S(34)}, !busy)) {
        Session* s = &session;
        app.Later([&app, s, id] { UpdateFromFile(app, *s, id); });
    }
    Paragraph(ui, Tr("Операции после импорта перестроятся на новой геометрии; ссылки на грани найдутся заново."), c.textMuted, ui.fonts.caption);
    ImGui::EndGroup();
}

} // namespace bz::app
