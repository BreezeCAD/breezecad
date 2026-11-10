#include "app/testing/UiProbe.h"

#include "app/AppContext.h"
#include "app/Notifications.h"
#include "app/Session.h"
#include "app/Workspace.h"
#include "app/features/OperationEditors.h"
#include "app/scripting/Scripting.h"
#include "app/sketch/SketchEditor.h"
#include "core/Log.h"
#include "core/PathText.h"
#include "document/SketchFeature.h"
#include "render/Viewport.h"
#include "ui/Widgets.h"

#if defined(IMGUI_ENABLE_TEST_ENGINE)
#include "app/testing/ImGuiProbeHooks.h"
#endif

#include <imgui.h>
#include <imgui_internal.h>
#include <nlohmann/json.hpp>

#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace bz::app::probe {
namespace {

using Json = nlohmann::json;

struct Item {
    ImGuiID     id = 0;
    ImRect      rect;
    std::string label;
    std::string window;
    bool        disabled = false;
    bool        checked = false;
    bool        opened = false;
    bool        hasLabel = false;
};

struct State {
    std::filesystem::path dir;
    bool                  on = false;
    std::vector<Item>     building;                    ///< the frame being built
    std::unordered_map<ImGuiID, std::size_t> index;    ///< id -> place in `building`
    std::vector<Item>     screen;                      ///< the last finished frame
    std::uint64_t         frames = 0;
};

State& S() {
    static State s;
    return s;
}

} // namespace
#if defined(IMGUI_ENABLE_TEST_ENGINE)
void InstallHooks();
#endif
namespace {

Json Rect(const ImRect& r) { return Json::array({r.Min.x, r.Min.y, r.Max.x, r.Max.y}); }

Json Items(const Json& request) {
    const std::string contains = request.value("contains", std::string());
    Json out = Json::array();
    for (const Item& it : S().screen) {
        if (!it.hasLabel) continue;   // nameless layout items say nothing to a test
        if (!contains.empty() && it.label.find(contains) == std::string::npos) continue;
        Json j = {{"label", it.label}, {"rect", Rect(it.rect)}, {"window", it.window}};
        if (it.disabled) j["disabled"] = true;
        if (it.checked) j["checked"] = true;
        if (it.opened) j["opened"] = true;
        out.push_back(std::move(j));
    }
    return out;
}

/// The view's rectangle on screen: the "##scene" button of the viewport panel.
bool SceneRect(ImRect& rect) {
    for (const Item& it : S().screen)
        if (it.label == "##scene") {
            rect = it.rect;
            return true;
        }
    return false;
}

Json Vec(Vec3 v) { return Json::array({v.x, v.y, v.z}); }

Json StateOf(AppContext& app) {
    Json j;
    const ImGuiIO& io = ImGui::GetIO();
    j["frames"] = S().frames;
    j["display"] = Json::array({io.DisplaySize.x, io.DisplaySize.y});
    j["fps"] = app.stats.fps;
    j["memory_mb"] = app.stats.memoryMb;
    j["home"] = app.workspace.ShowsHome();
    j["documents"] = app.workspace.sessions.size();
    j["popup_open"] = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    j["text_input"] = io.WantTextInput;
    Json notes = Json::array();
    for (const Notifications::Shown& n : app.notes.Current())
        notes.push_back({{"text", n.text}, {"kind", n.kind == Notifications::Kind::Error ? "error" : n.kind == Notifications::Kind::Success ? "success" : "info"}});
    j["notes"] = std::move(notes);
    ImRect scene;
    if (SceneRect(scene)) j["scene"] = Rect(scene);
    Session* s = app.Doc();
    if (!s || app.workspace.ShowsHome()) return j;
    j["title"] = s->Title();
    j["type"] = std::string(s->DocumentType());
    j["modified"] = s->IsModified();
    j["can_undo"] = s->CanUndo();
    j["can_redo"] = s->CanRedo();
    j["rebuilding"] = s->Rebuilding() || s->RebuildingInBackground();
    j["mode"] = s->mode == EditMode::Sketch ? "sketch" : s->mode == EditMode::PickPlane ? "pick_plane" : "model";
    j["selected_faces"] = s->selectedFaces.size();
    if (s->mode == EditMode::Sketch) {
        j["sketch_tool"] = InfoOf(app.sketch.Tool()).label;
        if (const auto* sk = s->doc.FindAs<doc::SketchFeature>(s->activeSketch)) {
            const Frame& f = sk->placement.frame;
            j["sketch"] = Json{{"name", sk->name}, {"origin", Vec(f.origin)}, {"x", Vec(f.xDir)}, {"y", Vec(f.YDir())}, {"normal", Vec(f.normal)}};
        }
    }
    if (OperationEditor* open = app.ops.Active()) {
        Json op = {{"open", true}};
        if (const doc::Feature* f = s->doc.Find(open->EditedFeature())) op["feature"] = f->name;
        j["operation"] = std::move(op);
    }
    return j;
}

Json Project(AppContext& app, const Json& request, bool sketch) {
    ImRect scene;
    if (!SceneRect(scene)) throw std::runtime_error("no 3D view on screen");
    const ViewProjection projection = app.viewport.Projection();
    Frame frame;
    if (sketch) {
        Session* s = app.Doc();
        const doc::SketchFeature* sk = s && s->mode == EditMode::Sketch ? s->doc.FindAs<doc::SketchFeature>(s->activeSketch) : nullptr;
        if (!sk) throw std::runtime_error("no sketch is being edited");
        frame = sk->placement.frame;
    }
    Json out = Json::array();
    for (const Json& p : request.at("points")) {
        const Vec3 world = sketch ? frame.ToWorld({p.at(0).get<double>(), p.at(1).get<double>()})
                                  : Vec3{p.at(0).get<double>(), p.at(1).get<double>(), p.at(2).get<double>()};
        const Vec2 local = projection.Project(world);
        out.push_back(Json::array({local.x + scene.Min.x, local.y + scene.Min.y}));
    }
    return {{"pixels", std::move(out)}, {"scene", Rect(scene)}, {"mm_per_pixel", projection.mmPerPixel}};
}

Json Answer(AppContext& app, const Json& request) {
    const std::string op = request.value("op", std::string());
    if (op == "items") return {{"items", Items(request)}};
    if (op == "state") return {{"state", StateOf(app)}};
    if (op == "project") return Project(app, request, false);
    if (op == "sketch_project") return Project(app, request, true);
    if (op == "run") return {{"reply", Json::parse(scripting::Run(app, request.at("request").dump()))}};
    throw std::runtime_error("unknown op: " + op);
}

} // namespace

void Enable(std::filesystem::path dir) {
    State& s = S();
    s.dir = std::move(dir);
    std::error_code ec;
    std::filesystem::create_directories(s.dir, ec);
    std::filesystem::remove(s.dir / "request.json", ec);
    std::filesystem::remove(s.dir / "reply.json", ec);
    s.on = true;
    ui::probeName = &Name;
#if defined(IMGUI_ENABLE_TEST_ENGINE)
    InstallHooks();
    if (ImGuiContext* g = ImGui::GetCurrentContext()) g->TestEngineHookItems = true;
    log::Info("Test probe: on, folder {}", ToUtf8(s.dir));
#else
    log::Info("Test probe: on without the item hooks (build with -DBZ_UI_PROBE=ON), folder {}", ToUtf8(s.dir));
#endif
}

bool Enabled() { return S().on; }

void Name(std::string_view kind, std::string_view text) {
    State& s = S();
    if (!s.on || s.building.empty()) return;
    Item& item = s.building.back();
    item.label = std::string(kind) + ":" + std::string(text);
    item.hasLabel = true;
}

void BeginFrame() {
    State& s = S();
    if (!s.on) return;
#if defined(IMGUI_ENABLE_TEST_ENGINE)
    ImGui::GetCurrentContext()->TestEngineHookItems = true;
#endif
    s.building.clear();
    s.index.clear();
}

void EndFrame() {
    State& s = S();
    if (!s.on) return;
    s.screen = s.building;
    ++s.frames;
}

bool Poll(AppContext& app) {
    State& s = S();
    if (!s.on) return false;
    const std::filesystem::path file = s.dir / "request.json";
    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) return false;
    std::stringstream text;
    text << std::ifstream(file).rdbuf();
    std::filesystem::remove(file, ec);
    Json reply;
    Json id;
    bool changed = false;   // only a command changes what is on screen; looking must not redraw
    try {
        const Json request = Json::parse(text.str());
        if (request.contains("id")) id = request["id"];
        changed = request.value("op", std::string()) == "run";
        reply = Answer(app, request);
        reply["ok"] = true;
    } catch (const std::exception& e) {
        reply = {{"ok", false}, {"error", e.what()}};
    }
    reply["id"] = id;
    const std::filesystem::path tmp = s.dir / "reply.tmp";
    {
        std::ofstream out(tmp, std::ios::binary);
        out << reply.dump(-1, ' ', false, Json::error_handler_t::replace);
    }
    std::filesystem::rename(tmp, s.dir / "reply.json", ec);
    return changed;
}

} // namespace bz::app::probe

// ---------------------------------------------------------------------------
// Dear ImGui tells about every item when it is built with -DBZ_UI_PROBE=ON
// (testing/ImGuiProbeHooks.cpp) and TestEngineHookItems is set (only by Enable).
#if defined(IMGUI_ENABLE_TEST_ENGINE)
namespace bz::app::probe {
namespace {

void OnItemAdd(ImGuiContext* ctx, ImGuiID id, const ImRect& bb, const ImGuiLastItemData* data) {
    State& s = S();
    if (!s.on || id == 0) return;
    Item item;
    item.id = id;
    item.rect = bb;
    if (ctx->CurrentWindow) {
        item.window = ctx->CurrentWindow->Name;
        // What the window clips away cannot be clicked: say where it really is.
        item.rect.ClipWith(ctx->CurrentWindow->ClipRect);
    }
    if (data) item.disabled = (data->ItemFlags & ImGuiItemFlags_Disabled) != 0;
    else item.disabled = (ctx->CurrentItemFlags & ImGuiItemFlags_Disabled) != 0;
    s.index[id] = s.building.size();
    s.building.push_back(std::move(item));
}

void OnItemInfo(ImGuiContext*, ImGuiID id, const char* label, ImGuiItemStatusFlags flags) {
    State& s = S();
    if (!s.on) return;
    const auto found = s.index.find(id);
    if (found == s.index.end()) return;
    Item& item = s.building[found->second];
    item.label = label ? label : "";
    item.hasLabel = true;
    item.checked = (flags & ImGuiItemStatusFlags_Checked) != 0;
    item.opened = (flags & ImGuiItemStatusFlags_Opened) != 0;
}

} // namespace

void InstallHooks() {
    probe_hooks::itemAdd = &OnItemAdd;
    probe_hooks::itemInfo = &OnItemInfo;
}

} // namespace bz::app::probe
#endif
