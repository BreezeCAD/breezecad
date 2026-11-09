// «Продолжить на устройстве» in the program (see HandoffUi.h).
#include "app/handoff/HandoffUi.h"

#include "app/AppContext.h"
#include "app/Autosave.h"
#include "app/Operations.h"
#include "app/Session.h"
#include "app/Settings.h"
#include "app/Workspace.h"
#include "app/features/OperationEditors.h"
#include "app/shell/Shell.h"
#include "assembly/Handoff.h"
#include "assembly/HandoffNet.h"
#include "assembly/ProjectPack.h"
#include "core/I18n.h"
#include "core/Log.h"
#include "core/PathText.h"
#include "core/Version.h"
#include "platform/OpenLinks.h"
#include "platform/Paths.h"
#include "ui/Ui.h"
#include "ui/Widgets.h"

#include <imgui.h>
#include <imgui_stdlib.h>
#include <qrcodegen.hpp>

#include <atomic>
#include <chrono>
#include <deque>
#include <format>
#include <functional>
#include <fstream>
#include <future>
#include <iterator>
#include <mutex>
#include <optional>
#include <thread>

namespace bz::app {

namespace fs = std::filesystem;
namespace ho = assembly::handoff;

namespace {

/// The computer's side: the window open while the device has the project.
struct Computer {
    enum class Stage { Waiting, Taken, Choosing };

    ho::Session                     session;
    fs::path                        dir;        ///< the handoff's own files (package, session, backups)
    std::string                     title;
    std::string                     address;    ///< shown under the QR
    std::optional<qrcodegen::QrCode> code;      ///< empty: the computer is in no network
    std::unique_ptr<ho::Server>     server;
    std::thread                     worker;     ///< serves the device: Serve() in a loop
    std::atomic<bool>               stop{false};
    std::mutex                      mutex;
    std::deque<ho::Server::Event>   events;     ///< from the worker, taken by the UI thread
    Stage                           stage = Stage::Waiting;
    int                             refused = 0;
    fs::path                        returned;   ///< the changes as they came
    std::vector<ho::Returned>       plan;
    std::vector<ho::Choice>         choices;

    ~Computer() {
        stop = true;
        if (worker.joinable()) worker.join();   // at most one Serve() wait
    }
};

/// The device's side: the project from the computer that is open here.
struct Device {
    ho::Address address;
    ho::Ticket  ticket;
    fs::path    folder;   ///< where it was unpacked; removed after the return
    std::string name;
};

} // namespace

struct HandoffState {
    std::unique_ptr<Computer> computer;

    bool        receiveOpen = false;
    std::string link;
    std::future<Expected<std::string>> fetching;
    std::optional<Device> device;
    std::future<Status>   giving;
};

namespace {

HandoffState& State(AppContext& app) {
    if (!app.shell.handoff) app.shell.handoff = std::make_shared<HandoffState>();
    return *app.shell.handoff;
}

fs::path WorkDir() {
    const fs::path dir = platform::ConfigDir() / "handoff";
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

Expected<std::string> ReadAll(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return Error{"Не удалось прочитать подготовленный проект"};
    return std::string(std::istreambuf_iterator<char>(in), {});
}

Status WriteAll(const fs::path& file, const std::string& bytes) {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!out) return Error{"Не удалось записать полученные данные (диск заполнен?)"};
    return {};
}

bool Inside(const fs::path& file, const fs::path& folder) {
    const fs::path rel = file.lexically_normal().lexically_relative(folder.lexically_normal());
    return !rel.empty() && *rel.begin() != "..";
}

bool Same(const fs::path& a, const fs::path& b) { return a.lexically_normal() == b.lexically_normal(); }

template <class T> bool Ready(const std::future<T>& f) {
    return f.valid() && f.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
}

/// Closes the open documents `which` picks (without asking: they are saved).
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

// --- The computer -------------------------------------------------------------

void ApplyChanges(AppContext& app) {
    HandoffState& st = State(app);
    if (!st.computer) return;
    Computer& c = *st.computer;
    const Expected<ho::ApplyReport> r = ho::ApplyReturn(c.session, c.returned, c.choices, c.dir / "Копии", Version());
    if (!r) {
        app.notes.Error(Trf("Изменения с устройства не применены: {}", Tr(r.GetError().message)));
        return;
    }
    // The documents the changes touched are opened again from their files:
    // the main one always (an assembly shows its parts as they are now).
    const fs::path main = c.session.files.front().source;
    const std::vector<fs::path> written = r->written;
    std::vector<fs::path> reopen = CloseWhere(app, [&](const fs::path& p) {
        if (Same(p, main)) return true;
        for (const fs::path& w : written)
            if (Same(p, w)) return true;
        return false;
    });
    for (auto it = reopen.rbegin(); it != reopen.rend(); ++it) app.workspace.Open(*it, app.notes, app.settings);
    app.notes.Success(Trf("Изменения с устройства на месте: файлов {}", r->written.size()));
    st.computer.reset();   // the device has given its copy up: the server stops
}

void TakeEvents(AppContext& app, Computer& c) {
    std::deque<ho::Server::Event> events;
    {
        const std::lock_guard lock(c.mutex);
        events.swap(c.events);
    }
    for (ho::Server::Event& e : events) {
        switch (e.kind) {
        case ho::Server::Event::Kind::Sent:
            c.stage = Computer::Stage::Taken;
            break;
        case ho::Server::Event::Kind::Refused:
            ++c.refused;
            break;
        case ho::Server::Event::Kind::Returned: {
            if (c.stage == Computer::Stage::Choosing) break;   // one return at a time
            c.returned = c.dir / "back.zip";
            if (Status st = WriteAll(c.returned, e.data); !st) {
                app.notes.Error(Tr(st.GetError().message));
                break;
            }
            Expected<std::vector<ho::Returned>> plan = ho::PlanReturn(c.session, c.returned);
            if (!plan) {
                app.notes.Error(Trf("Изменения с устройства не приняты: {}", Tr(plan.GetError().message)));
                break;
            }
            if (plan->empty()) {
                app.notes.Info(Tr("Проект вернулся с устройства без изменений"));
                app.Later([&app] { State(app).computer.reset(); });
                break;
            }
            c.plan = std::move(*plan);
            c.choices.clear();
            bool conflict = false;
            for (const ho::Returned& r : c.plan) {
                conflict = conflict || r.change == ho::Change::Conflict;
                c.choices.push_back(r.change == ho::Change::Conflict ? ho::Choice::KeepBoth : ho::Choice::TakeDevice);
            }
            if (conflict) c.stage = Computer::Stage::Choosing;
            else app.Later([&app] { ApplyChanges(app); });
            break;
        }
        case ho::Server::Event::Kind::None:
            break;
        }
    }
}

/// The QR, black on white whatever the theme (cameras read it best so).
void DrawQr(ui::Ui& ui, const qrcodegen::QrCode& qr, float side) {
    const float avail = ImGui::GetContentRegionAvail().x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (avail - side) / 2));
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const int quiet = 3;   // the white margin the standard asks (in modules)
    const int n = qr.getSize();
    const float m = side / static_cast<float>(n + 2 * quiet);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, {p.x + side, p.y + side}, IM_COL32_WHITE, ui.S(8));
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x)
            if (qr.getModule(x, y)) {
                const ImVec2 a{p.x + static_cast<float>(x + quiet) * m, p.y + static_cast<float>(y + quiet) * m};
                dl->AddRectFilled(a, {a.x + m + 0.5f, a.y + m + 0.5f}, IM_COL32_BLACK);
            }
    ImGui::Dummy({side, side});
}

void WrappedText(ui::Ui& ui, const std::string& text, bool secondary = true) {
    ui::FontScope font(ui.fonts.body);
    if (secondary) ImGui::PushStyleColor(ImGuiCol_Text, ui::ToVec4(ui.C().textSecondary));
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopTextWrapPos();
    if (secondary) ImGui::PopStyleColor();
}

void DrawComputer(AppContext& app, HandoffState& st) {
    ui::Ui& ui = app.ui;
    Computer& c = *st.computer;
    TakeEvents(app, c);
    if (!st.computer) return;
    bool open = true;
    if (!BeginDialog(ui, "##handoff-send", open, ui.S(440))) {
        st.computer.reset();
        return;
    }
    {
        ui::FontScope font(ui.fonts.title);
        ImGui::TextUnformatted(Tr("Продолжить на устройстве"));
    }
    if (!c.code) {
        WrappedText(ui, Tr("Компьютер не подключён к сети: устройство его не найдёт. Подключитесь к Wi-Fi или к сети и попробуйте снова."));
    } else if (c.stage == Computer::Stage::Waiting) {
        WrappedText(ui, Trf("Наведите камеру телефона или планшета на код — откроется BreezeCAD с «{}». Устройство должно быть в той же сети.", c.title));
        ImGui::Dummy({0, ui.S(4)});
        DrawQr(ui, *c.code, ui.S(240));
        {
            ui::FontScope font(ui.fonts.mono);
            const float w = ImGui::CalcTextSize(c.address.c_str()).x;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (ImGui::GetContentRegionAvail().x - w) / 2));
            ImGui::TextDisabled("%s", c.address.c_str());
        }
#if defined(_WIN32)
        WrappedText(ui, Tr("Если Windows спросит о доступе к сети — разрешите для частных сетей."));
#endif
        if (ImGui::SmallButton(Tr("Скопировать ссылку"))) ImGui::SetClipboardText(ho::QrText(c.server->GetAddress()).c_str());
    } else if (c.stage == Computer::Stage::Taken) {
        WrappedText(ui, Trf("«{}» на устройстве. Когда закончите, нажмите там «Вернуть на компьютер» — изменения придут сюда. Не закрывайте это окно.", c.title),
                    false);
    } else {
        WrappedText(ui, Tr("Эти файлы изменились и здесь, и на устройстве. Что оставить?"), false);
        int others = 0;
        for (std::size_t i = 0; i < c.plan.size(); ++i) {
            if (c.plan[i].change != ho::Change::Conflict) {
                ++others;
                continue;
            }
            ImGui::PushID(static_cast<int>(i));
            ImGui::TextUnformatted(ToUtf8(c.plan[i].target.filename()).c_str());
            int choice = static_cast<int>(c.choices[i]);
            ImGui::RadioButton(Tr("С устройства"), &choice, static_cast<int>(ho::Choice::TakeDevice));
            ImGui::SameLine();
            ImGui::RadioButton(Tr("Своё"), &choice, static_cast<int>(ho::Choice::KeepMine));
            ImGui::SameLine();
            ImGui::RadioButton(Tr("Оба"), &choice, static_cast<int>(ho::Choice::KeepBoth));
            c.choices[i] = static_cast<ho::Choice>(choice);
            ImGui::PopID();
        }
        if (others > 0) WrappedText(ui, Trf("Остальные изменения применятся сами: файлов {}", others));
    }
    if (c.refused > 0) WrappedText(ui, Trf("Отклонено подключений с чужим кодом: {}", c.refused));

    ImGui::Dummy({0, ui.S(8)});
    const float width = ImGui::GetContentRegionAvail().x;
    bool close = false;
    if (c.stage == Computer::Stage::Choosing) {
        const float bw = (width - ui.S(8)) / 2;
        if (ui::PrimaryButton(ui, "##handoff-apply", Tr("Применить"), nullptr, {bw, ui.S(36)})) app.Later([&app] { ApplyChanges(app); });
        ImGui::SameLine(0, ui.S(8));
        close = ui::SecondaryButton(ui, "##handoff-cancel", Tr("Отмена"), "Esc", {bw, ui.S(36)});
    } else {
        close = ui::SecondaryButton(ui, "##handoff-close", c.stage == Computer::Stage::Taken ? Tr("Прекратить передачу") : Tr("Закрыть"), "Esc",
                                    {width, ui.S(36)});
    }
    if (close || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        ImGui::CloseCurrentPopup();
        app.Later([&app] { State(app).computer.reset(); });
    }
    ImGui::EndPopup();
}

// --- The device ---------------------------------------------------------------

void StartReceive(AppContext& app, const std::string& link) {
    HandoffState& st = State(app);
    if (st.fetching.valid()) return;   // one at a time
    if (st.device) {
        app.notes.Error(Tr("Сначала верните на компьютер проект, открытый с него"));
        return;
    }
    Expected<ho::Address> address = ho::ParseQr(link);
    if (!address) {
        app.notes.Error(Tr(address.GetError().message));
        return;
    }
    const ho::Address a = *address;
    st.fetching = std::async(std::launch::async, [a] { return ho::Fetch(a); });
    st.device = Device{a, {}, {}, {}};   // remembered for when it comes; not open here until it has a folder
    app.notes.Info(Tr("Забираю проект с компьютера…"));
}

void FinishReceive(AppContext& app, HandoffState& st) {
    Expected<std::string> bytes = st.fetching.get();
    const ho::Address address = st.device ? st.device->address : ho::Address{};
    st.device.reset();
    if (!bytes) {
        app.notes.Error(Trf("Проект не получен: {}", Tr(bytes.GetError().message)));
        return;
    }
    const fs::path package = WorkDir() / "received.bzp";
    if (Status w = WriteAll(package, *bytes); !w) {
        app.notes.Error(Tr(w.GetError().message));
        return;
    }
    Expected<ho::Ticket> ticket = ho::ReadTicket(package);
    if (!ticket) {
        app.notes.Error(Trf("Проект не получен: {}", Tr(ticket.GetError().message)));
        return;
    }
    const std::string name = ToUtf8(PathFromUtf8(ticket->main).stem());
    const fs::path folder = platform::DocumentsDir() / PathFromUtf8("С компьютера") / PathFromUtf8(name);
    // The folder is the handoff's own: what an earlier handoff left there goes.
    std::error_code ec;
    fs::remove_all(folder, ec);
    Expected<fs::path> main = assembly::UnpackProject(package, folder);
    fs::remove(package, ec);
    if (!main) {
        app.notes.Error(Trf("Проект не распакован: {}", Tr(main.GetError().message)));
        return;
    }
    st.device = Device{address, std::move(*ticket), folder, name};
    const fs::path open = *main;
    app.Later([&app, open] { OpenDocument(app, open); });
    app.notes.Success(Trf("«{}» с компьютера открыт. Закончите — нажмите «Вернуть на компьютер».", name));
}

void FinishReturn(AppContext& app, HandoffState& st) {
    const Status sent = st.giving.get();
    if (!sent) {
        app.notes.Error(Trf("Изменения не отправлены: {} Окно передачи на компьютере должно быть открыто.", Tr(sent.GetError().message)));
        return;
    }
    app.Later([&app] {
        HandoffState& s = State(app);
        if (!s.device) return;
        const fs::path folder = s.device->folder;
        CloseWhere(app, [&](const fs::path& p) { return Inside(p, folder); });
        if (Status d = ho::DropCopy(folder); !d) app.notes.Error(Tr(d.GetError().message));
        s.device.reset();
        app.notes.Success(Tr("Изменения на компьютере, копия на устройстве удалена"));
    });
}

void DrawReceiveDialog(AppContext& app, HandoffState& st) {
    ui::Ui& ui = app.ui;
    if (!BeginDialog(ui, "##handoff-receive", st.receiveOpen, ui.S(460))) return;
    {
        ui::FontScope font(ui.fonts.title);
        ImGui::TextUnformatted(Tr("Принять с компьютера"));
    }
    WrappedText(ui, Tr("На компьютере: «Продолжить на устройстве». Отсканируйте его QR-код камерой — BreezeCAD откроется сам. Или вставьте сюда ссылку из того окна."));
    const float width = ImGui::GetContentRegionAvail().x;
    ImGui::SetNextItemWidth(width);
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    const bool enter = ImGui::InputTextWithHint("##handoff-link", "breezecad://handoff?…", &st.link, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::Dummy({0, ui.S(8)});
    const float bw = (width - ui.S(8)) / 2;
    const bool accept = ui::PrimaryButton(ui, "##handoff-take", Tr("Принять"), nullptr, {bw, ui.S(36)}, !st.link.empty()) || (enter && !st.link.empty());
    ImGui::SameLine(0, ui.S(8));
    const bool cancel = ui::SecondaryButton(ui, "##handoff-no", Tr("Отмена"), "Esc", {bw, ui.S(36)}) || ImGui::IsKeyPressed(ImGuiKey_Escape);
    if (accept || cancel) {
        const std::string link = st.link;
        st.link.clear();
        st.receiveOpen = false;
        ImGui::CloseCurrentPopup();
        if (accept) StartReceive(app, link);
    }
    ImGui::EndPopup();
}

/// A small bar while a project from a computer is open: it says so and returns it.
void DrawDeviceBar(AppContext& app, HandoffState& st) {
    if (!st.device || st.device->folder.empty() || ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) return;
    ui::Ui& ui = app.ui;
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos({display.x / 2, display.y - ui.S(72)}, ImGuiCond_Always, {0.5f, 1.0f});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ui.S(12, 8));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, ui.S(12));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    if (ImGui::Begin("##handoff-bar", nullptr, flags)) {
        ui::FontScope font(ui.fonts.body);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(Trf("С компьютера: «{}»", st.device->name).c_str());
        ImGui::SameLine(0, ui.S(12));
        const bool busy = st.giving.valid();
        if (ui::PrimaryButton(ui, "##handoff-return", busy ? Tr("Отправляю…") : Tr("Вернуть на компьютер"), nullptr, {ui.S(190), ui.S(32)}, !busy))
            ReturnHandoff(app);
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
}

} // namespace

bool CanStartHandoff(AppContext& app) {
    const Session* s = app.workspace.ActiveTab();
    return s && !app.workspace.ShowsHome() && !(app.shell.handoff && app.shell.handoff->computer);
}

void StartHandoff(AppContext& app) {
    Session* s = app.workspace.ActiveTab();
    if (!s || app.workspace.ShowsHome()) return;
    if (s->host) s = s->host;
    app.ops.Commit(app, *s);
    // The device gets the files as saved: unsaved changes are saved first.
    if (s->path.empty() || s->IsModified()) {
        if (!app.workspace.Save(*s, false, app.notes, app.settings)) return;
        app.autosave.Discard(*s);
    }
    auto c = std::make_unique<Computer>();
    c->dir = WorkDir();
    const fs::path package = c->dir / "send.bzp";
    Expected<ho::Session> session = ho::Send(s->path, package, Version());
    if (!session) {
        app.notes.Error(Trf("Проект не подготовлен к передаче: {}", Tr(session.GetError().message)));
        return;
    }
    // Kept on disk too: what went where, should the program be closed meanwhile.
    if (Status saved = ho::SaveSession(*session, c->dir / "session.json"); !saved) log::Warn("handoff: {}", saved.GetError().message);
    Expected<std::string> bytes = ReadAll(package);
    if (!bytes) {
        app.notes.Error(Tr(bytes.GetError().message));
        return;
    }
    Expected<ho::Server> server = ho::Server::Start(*session, std::move(*bytes));
    if (!server) {
        app.notes.Error(Tr(server.GetError().message));
        return;
    }
    c->session = std::move(*session);
    c->title = s->Title();
    c->server = std::make_unique<ho::Server>(std::move(*server));
    const ho::Address& a = c->server->GetAddress();
    if (!a.host.empty()) {
        c->address = std::format("{}:{}", a.host, a.port);
        c->code = qrcodegen::QrCode::encodeText(ho::QrText(a).c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
        Computer* raw = c.get();
        c->worker = std::thread([raw] {
            while (!raw->stop) {
                ho::Server::Event e = raw->server->Serve(250);
                if (e.kind == ho::Server::Event::Kind::None) continue;
                const std::lock_guard lock(raw->mutex);
                raw->events.push_back(std::move(e));
            }
        });
    }
    State(app).computer = std::move(c);
}

void OpenHandoffReceive(AppContext& app) { State(app).receiveOpen = true; }

bool HandoffOnDevice(const AppContext& app) {
    return app.shell.handoff && app.shell.handoff->device && !app.shell.handoff->device->folder.empty();
}

void ReturnHandoff(AppContext& app) {
    HandoffState& st = State(app);
    if (!st.device || st.device->folder.empty() || st.giving.valid()) return;
    const Device& d = *st.device;
    // What is open from the project is saved first: the computer gets the files.
    for (const auto& session : app.workspace.sessions) {
        Session& s = *session;
        if (s.path.empty() || !Inside(s.path, d.folder)) continue;
        app.ops.Commit(app, s);
        if (s.IsModified()) {
            if (!app.workspace.Save(s, false, app.notes, app.settings)) return;
            app.autosave.Discard(s);
        }
    }
    const fs::path out = WorkDir() / "return.zip";
    Expected<int> count = ho::PackReturn(d.ticket, d.folder, out, Version());
    if (!count) {
        app.notes.Error(Trf("Изменения не собраны: {}", Tr(count.GetError().message)));
        return;
    }
    Expected<std::string> bytes = ReadAll(out);
    std::error_code ec;
    fs::remove(out, ec);
    if (!bytes) {
        app.notes.Error(Tr(bytes.GetError().message));
        return;
    }
    const ho::Address a = d.address;
    st.giving = std::async(std::launch::async, [a, data = std::move(*bytes)] { return ho::Give(a, data); });
}

void DrawHandoff(AppContext& app) {
    while (std::optional<std::string> link = platform::TakeOpenedLink()) StartReceive(app, *link);
    if (!app.shell.handoff) return;
    HandoffState& st = *app.shell.handoff;
    if (Ready(st.fetching)) FinishReceive(app, st);
    if (Ready(st.giving)) FinishReturn(app, st);
    if (st.computer) DrawComputer(app, st);
    if (st.receiveOpen) DrawReceiveDialog(app, st);
    DrawDeviceBar(app, st);
}

} // namespace bz::app
