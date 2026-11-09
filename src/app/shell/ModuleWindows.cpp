#include "app/shell/ModuleWindows.h"

#include "app/Actions.h"
#include "app/AppContext.h"
#include "app/shell/FilePreviews.h"
#include "app/shell/Shell.h"
#include "core/I18n.h"
#include "core/Image.h"
#include "core/Log.h"
#include "core/PathText.h"
#include "modules/ModuleHost.h"
#include "modules/ModuleManager.h"
#include "ui/Gl.h"
#include "ui/Menus.h"
#include "ui/Ui.h"
#include "ui/Widgets.h"

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F   // OpenGL 1.2; Windows headers stop at 1.1
#endif

namespace bz::app {

namespace {

using Json = nlohmann::json;

// --- what the program keeps of the modules' windows ----------------------------

struct Choice {
    int sent  = 0;   ///< the option the module sent last (its number)
    int value = 0;   ///< what the user chose (the module's value until then)
};

struct Picture {
    GLuint texture = 0;
    int    width = 0, height = 0;
};

struct PanelState {
    std::string module, id, json;   ///< as in ModuleHost, `json` to see a change
    Json        panel;
    std::map<std::string, Choice> choices;
};

struct State {
    std::uint64_t actionsRevision = ~std::uint64_t{0};
    std::vector<std::string> registered;      ///< the action ids added to app.actions
    std::uint64_t panelsRevision = ~std::uint64_t{0};
    std::vector<PanelState> panels;
    std::map<std::string, Picture> pictures;  ///< "image" items: by their base64 text
};

State& St() {
    static State state;   // one program, one set of module windows
    return state;
}

std::string Text(const Json& j, const char* key) {
    const auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

bool Flag(const Json& j, const char* key) {
    const auto it = j.find(key);
    if (it == j.end()) return false;
    if (it->is_boolean()) return it->get<bool>();
    if (it->is_number()) return it->get<double>() != 0;
    return false;
}

/// The item's caption: "label" (ADR-0011, sdk/bz_panel.h), "text" taken too.
std::string Caption(const Json& j) {
    std::string label = Text(j, "label");
    return label.empty() ? Text(j, "text") : label;
}

/// "Esc", "esc", "ESC" -> "esc".
std::string Lower(std::string text) {
    for (char& ch : text) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return text;
}

float Number(const Json& j, const char* key, float fallback) {
    const auto it = j.find(key);
    return it != j.end() && it->is_number() ? it->get<float>() : fallback;
}

const modules::ModuleAction* FindAction(std::string_view module, std::string_view id) {
    for (const modules::ModuleAction& a : modules::ModuleActions())
        if (a.module == module && a.id == id) return &a;
    return nullptr;
}

/// Calls into the module after the frame: it may show or close panels then.
void Notify(AppContext& app, std::string module, std::string eventJson) {
    app.Later([&app, module = std::move(module), eventJson = std::move(eventJson)] {
        if (!app.modules) return;
        app.modules->Call(module, [&](const BzModuleApi& api) {
            if (BZ_HAS(&api, on_panel_event)) api.on_panel_event(eventJson.c_str());
        });
    });
}

void SendEvent(AppContext& app, const PanelState& p, const std::string& id) {
    Json values = Json::object();
    for (const auto& [key, c] : p.choices) values[key] = c.value;
    Notify(app, p.module, Json{{"panel", p.id}, {"id", id}, {"values", values}}.dump());
}

// --- pictures -----------------------------------------------------------------

std::vector<std::uint8_t> FromBase64(std::string_view text) {
    std::vector<std::uint8_t> out;
    out.reserve(text.size() * 3 / 4);
    std::uint32_t bits = 0;
    int count = 0;
    for (const char ch : text) {
        int v;
        if (ch >= 'A' && ch <= 'Z') v = ch - 'A';
        else if (ch >= 'a' && ch <= 'z') v = ch - 'a' + 26;
        else if (ch >= '0' && ch <= '9') v = ch - '0' + 52;
        else if (ch == '+' || ch == '-') v = 62;
        else if (ch == '/' || ch == '_') v = 63;
        else continue;   // '=', line breaks
        bits = (bits << 6) | static_cast<std::uint32_t>(v);
        count += 6;
        if (count >= 8) {
            count -= 8;
            out.push_back(static_cast<std::uint8_t>((bits >> count) & 0xFF));
        }
    }
    return out;
}

const Picture& PictureOf(const std::string& base64) {
    auto& pictures = St().pictures;
    if (const auto it = pictures.find(base64); it != pictures.end()) return it->second;
    Picture pic;
    const std::vector<std::uint8_t> bytes = FromBase64(base64);
    if (Expected<Image> image = DecodeImage(bytes); image) {
        const Image small = FitWithin(*image, 1024);   // a module's picture, not a texture atlas
        glGenTextures(1, &pic.texture);
        glBindTexture(GL_TEXTURE_2D, pic.texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, small.width, small.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, small.rgba.data());
        pic.width  = small.width;
        pic.height = small.height;
    } else {
        log::Warn("Модуль прислал картинку, которая не читается: {}", image.GetError().message);
    }
    return pictures.emplace(base64, pic).first->second;
}

/// Pictures no panel shows any more.
void DropPictures() {
    State& st = St();
    for (auto it = st.pictures.begin(); it != st.pictures.end();) {
        bool used = false;
        for (const PanelState& p : st.panels)
            if (p.json.find(it->first) != std::string::npos) used = true;
        if (used) {
            ++it;
            continue;
        }
        if (it->second.texture) glDeleteTextures(1, &it->second.texture);
        it = st.pictures.erase(it);
    }
}

// --- panels -------------------------------------------------------------------

/// The panels as the modules have them now; the user's choices are kept.
void SyncPanels() {
    State& st = St();
    if (st.panelsRevision == modules::ModuleUiRevision()) return;
    st.panelsRevision = modules::ModuleUiRevision();
    std::vector<PanelState> fresh;
    for (const modules::ModulePanel& mp : modules::ModulePanels()) {
        PanelState p;
        const auto old = std::find_if(st.panels.begin(), st.panels.end(),
                                      [&](const PanelState& o) { return o.module == mp.module && o.id == mp.id; });
        if (old != st.panels.end()) p = std::move(*old);
        p.module = mp.module;
        p.id     = mp.id;
        if (p.json != mp.json || p.panel.is_null()) {
            p.json  = mp.json;
            p.panel = Json::parse(mp.json, nullptr, false);
            if (!p.panel.is_object()) p.panel = Json::object();
            // A choice the module sent anew takes over; one it did not change keeps the user's pick.
            std::map<std::string, Choice> choices;
            if (const auto items = p.panel.find("items"); items != p.panel.end() && items->is_array())
                for (const Json& item : *items) {
                    if (Text(item, "type") != "choice") continue;
                    const std::string id = Text(item, "id");
                    if (id.empty()) continue;
                    const int sent = static_cast<int>(Number(item, "value", 0));
                    Choice c{sent, sent};
                    if (const auto was = p.choices.find(id); was != p.choices.end() && was->second.sent == sent) c.value = was->second.value;
                    choices[id] = c;
                }
            p.choices = std::move(choices);
        }
        fresh.push_back(std::move(p));
    }
    st.panels = std::move(fresh);
    DropPictures();
}

void WrappedText(ui::Ui& ui, const std::string& text, const Color& color) {
    ImGui::PushStyleColor(ImGuiCol_Text, ui::ToVec4(color));
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

void Centre(float width) {
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (ImGui::GetContentRegionAvail().x - width) / 2));
}

/// A matrix of '0' / '1' rows (a QR code the module made): black on white,
/// with a white margin, whatever the theme — a camera has to read it.
void DrawBitmap(ui::Ui& ui, const Json& item) {
    const auto rows = item.find("modules");
    if (rows == item.end() || !rows->is_array() || rows->empty()) return;
    const int n = static_cast<int>(rows->size());
    const float side = ui.S(Number(item, "size", 220));
    Centre(side);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const int quiet = 3;
    const float m = side / static_cast<float>(n + 2 * quiet);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, {p.x + side, p.y + side}, IM_COL32_WHITE, ui.S(8));
    for (int y = 0; y < n; ++y) {
        const Json& row = (*rows)[static_cast<std::size_t>(y)];
        if (!row.is_string()) continue;
        const std::string& bits = row.get_ref<const std::string&>();
        for (int x = 0; x < static_cast<int>(bits.size()) && x < n; ++x)
            if (bits[static_cast<std::size_t>(x)] == '1') {
                const ImVec2 a{p.x + static_cast<float>(x + quiet) * m, p.y + static_cast<float>(y + quiet) * m};
                dl->AddRectFilled(a, {a.x + m + 0.5f, a.y + m + 0.5f}, IM_COL32_BLACK);
            }
    }
    ImGui::Dummy({side, side});
}

void DrawImage(ui::Ui& ui, const Json& item) {
    const Picture pic = PictureOf(Text(item, "png"));
    if (!pic.texture || pic.width <= 0) return;
    const float width = std::min(ui.S(Number(item, "width", static_cast<float>(pic.width))), ImGui::GetContentRegionAvail().x);
    const float height = width * static_cast<float>(pic.height) / static_cast<float>(pic.width);
    Centre(width);
    ImGui::Image(static_cast<ImTextureID>(pic.texture), {width, height});
}

void DrawStatus(ui::Ui& ui, const Json& item) {
    const ui::ThemeColors& c = ui.C();
    const bool busy = Flag(item, "busy");
    ImGui::AlignTextToFramePadding();
    if (busy) {   // a turning arc before the text
        const float r = ui.S(6);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const ImVec2 centre{p.x + r, p.y + ImGui::GetFrameHeight() / 2};
        const float start = static_cast<float>(ImGui::GetTime()) * 6.0f;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->PathArcTo(centre, r, start, start + 4.2f, 16);
        dl->PathStroke(ui::ToU32(c.accent), 0, ui.S(2));
        ImGui::Dummy({2 * r, ImGui::GetFrameHeight()});
        ImGui::SameLine(0, ui.S(8));
    }
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, ui::ToVec4(c.text));
    ImGui::TextUnformatted(Text(item, "text").c_str());
    ImGui::PopStyleColor();
}

void DrawNote(ui::Ui& ui, const Json& item) {
    const ui::ThemeColors& c = ui.C();
    const std::string icon = Text(item, "icon");
    const float iconSize = ui.S(16);
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float left = icon.empty() ? 0 : iconSize + ui.S(8);
    if (!icon.empty()) ui.icons.Draw(ImGui::GetWindowDrawList(), icon, at, iconSize, c.textSecondary);
    ImGui::SetCursorScreenPos({at.x + left, at.y});
    ImGui::BeginGroup();
    WrappedText(ui, Text(item, "text"), c.textSecondary);
    ImGui::EndGroup();
}

void DrawChoice(AppContext& app, PanelState& p, const Json& item, int index) {
    ui::Ui& ui = app.ui;
    const ui::ThemeColors& c = ui.C();
    const std::string id = Text(item, "id");
    const auto it = p.choices.find(id);
    const auto options = item.find("options");
    if (it == p.choices.end() || options == item.end() || !options->is_array() || options->empty()) return;
    std::vector<std::string> labels;   // "options": ["С устройства", "Своё", "Оба"]
    for (const Json& o : *options) labels.push_back(o.is_string() ? o.get<std::string>() : Caption(o));
    ImGui::PushID(index);
    // The file's picture, when the module names a saved part or assembly.
    if (const std::string preview = Text(item, "preview"); !preview.empty()) {
        const FilePreview pic = FilePreviewOf(PathFromUtf8(preview));
        const float side = ui.S(48);
        const ImVec2 at = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(at, {at.x + side, at.y + side}, ui::ToU32(c.hover), ui.S(6));
        if (pic.texture && pic.width > 0 && pic.height > 0) {
            const float k = std::min(side / static_cast<float>(pic.width), side / static_cast<float>(pic.height));
            const float w = static_cast<float>(pic.width) * k, h = static_cast<float>(pic.height) * k;
            const ImVec2 a{at.x + (side - w) / 2, at.y + (side - h) / 2};
            dl->AddImage(static_cast<ImTextureID>(pic.texture), a, {a.x + w, a.y + h});
        }
        ImGui::Dummy({side, side});
        ImGui::SameLine(0, ui.S(10));
    }
    ImGui::BeginGroup();
    ImGui::TextUnformatted(Caption(item).c_str());
    std::vector<const char*> names;
    for (const std::string& l : labels) names.push_back(l.c_str());
    const int count   = static_cast<int>(names.size());
    const int current = std::clamp(it->second.value, 0, count - 1);
    const int chosen  = ui::Segmented(ui, "##choice", names, current, count, ImGui::GetContentRegionAvail().x);
    if (chosen >= 0 && chosen < count) it->second.value = chosen;
    ImGui::EndGroup();
    ImGui::PopID();
}

/// The buttons of a row; returns the id of the one pressed (or the one whose key went down).
std::string DrawButtons(ui::Ui& ui, const Json& item, int index) {
    const auto buttons = item.find("items");
    if (buttons == item.end() || !buttons->is_array() || buttons->empty()) return {};
    std::string pressed;
    const float gap = ui.S(8);
    const float count = static_cast<float>(buttons->size());
    const float width = (ImGui::GetContentRegionAvail().x - gap * (count - 1)) / count;
    ImGui::PushID(index);
    int i = 0;
    for (const Json& b : *buttons) {
        if (i > 0) ImGui::SameLine(0, gap);
        const std::string id = Text(b, "id"), label = Caption(b), key = Lower(Text(b, "key"));
        const char* hint = key == "enter" ? "Enter" : key == "esc" ? "Esc" : nullptr;
        const std::string imguiId = "##btn" + std::to_string(i);
        const bool enabled = !b.contains("enabled") || Flag(b, "enabled");
        const bool clicked = Text(b, "style") == "primary"
                                 ? ui::PrimaryButton(ui, imguiId.c_str(), label.c_str(), hint, {width, ui.S(36)}, enabled)
                                 : ui::SecondaryButton(ui, imguiId.c_str(), label.c_str(), hint, {width, ui.S(36)}, enabled);
        const bool keyed = enabled && ((key == "enter" && (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))) ||
                                       (key == "esc" && ImGui::IsKeyPressed(ImGuiKey_Escape)));
        if ((clicked || keyed) && pressed.empty()) pressed = id;
        ++i;
    }
    ImGui::PopID();
    return pressed;
}

/// Whether some button of the panel answers Esc itself.
bool HasEscButton(const Json& panel) {
    const auto items = panel.find("items");
    if (items == panel.end() || !items->is_array()) return false;
    for (const Json& item : *items) {
        if (Text(item, "type") != "buttons") continue;
        if (const auto b = item.find("items"); b != item.end() && b->is_array())
            for (const Json& button : *b)
                if (Lower(Text(button, "key")) == "esc") return true;
    }
    return false;
}

void DrawTitle(ui::Ui& ui, const Json& panel) {
    const ui::ThemeColors& c = ui.C();
    const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    {
        ui::FontScope font(ui.fonts.title);
        ImGui::TextUnformatted(Text(panel, "title").c_str());
    }
    // The module's name, so the user sees who asks: a chip on the right.
    if (const std::string badge = Text(panel, "badge"); !badge.empty()) {
        ImFont* font = ui.fonts.caption;
        const float fs = ui.S(11);
        const ImVec2 size = font->CalcTextSizeA(fs, FLT_MAX, 0, badge.c_str());
        const ImVec2 pad = ui.S(8, 3);
        const float top = ImGui::GetItemRectMin().y + (ImGui::GetItemRectSize().y - size.y - 2 * pad.y) / 2;
        const ImVec2 a{right - size.x - 2 * pad.x, top};
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(a, {right, top + size.y + 2 * pad.y}, ui::ToU32(c.accentSoft), ui.S(10));
        dl->AddText(font, fs, {a.x + pad.x, a.y + pad.y}, ui::ToU32(c.accent), badge.c_str());
    }
}

/// One panel; false when the user closed it.
bool DrawPanel(AppContext& app, PanelState& p) {
    ui::Ui& ui = app.ui;
    const ui::ThemeColors& c = ui.C();
    const std::string popup = "##module-" + p.module + "-" + p.id;
    bool open = true;
    if (!BeginDialog(ui, popup.c_str(), open, ui.S(Number(p.panel, "width", 440)))) return false;
    DrawTitle(ui, p.panel);
    ImGui::Dummy({0, ui.S(2)});
    std::string pressed;
    if (const auto items = p.panel.find("items"); items != p.panel.end() && items->is_array()) {
        int index = 0;
        for (const Json& item : *items) {
            if (!item.is_object()) continue;
            const std::string type = Text(item, "type");
            ImGui::PushID(index);
            if (type == "text") {
                WrappedText(ui, Text(item, "text"), Flag(item, "secondary") ? c.textSecondary : c.text);
            } else if (type == "mono") {
                ui::FontScope font(ui.fonts.mono);
                const std::string text = Text(item, "text");
                Centre(ImGui::CalcTextSize(text.c_str()).x);
                WrappedText(ui, text, c.textSecondary);
            } else if (type == "note") {
                DrawNote(ui, item);
            } else if (type == "status") {
                DrawStatus(ui, item);
            } else if (type == "link") {
                const std::string text = Caption(item);
                if (ui::LinkButton(ui, ("##link" + std::to_string(index)).c_str(), text.c_str()) && pressed.empty()) pressed = Text(item, "id");
            } else if (type == "button") {   // one button on its own row; "style": "link" — as a link
                const std::string text = Caption(item), style = Text(item, "style"), imguiId = "##button" + std::to_string(index);
                const bool clicked = style == "link"      ? ui::LinkButton(ui, imguiId.c_str(), text.c_str())
                                     : style == "primary" ? ui::PrimaryButton(ui, imguiId.c_str(), text.c_str(), nullptr, {ImGui::GetContentRegionAvail().x, ui.S(36)})
                                                          : ui::SecondaryButton(ui, imguiId.c_str(), text.c_str(), nullptr, {ImGui::GetContentRegionAvail().x, ui.S(36)});
                if (clicked && pressed.empty()) pressed = Text(item, "id");
            } else if (type == "bitmap") {
                DrawBitmap(ui, item);
            } else if (type == "image") {
                DrawImage(ui, item);
            } else if (type == "choice") {
                DrawChoice(app, p, item, index);
            } else if (type == "buttons") {
                ImGui::Dummy({0, ui.S(4)});
                if (std::string b = DrawButtons(ui, item, index); !b.empty() && pressed.empty()) pressed = std::move(b);
            }
            ImGui::PopID();
            ++index;
        }
    }
    bool closed = false;
    if (!pressed.empty()) {
        SendEvent(app, p, pressed);
    } else if (!HasEscButton(p.panel) && ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        closed = true;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
    return !closed;
}

} // namespace

std::string ModuleActionId(std::string_view module, std::string_view id) {
    return "module." + std::string(module) + "." + std::string(id);
}

void SyncModuleActions(AppContext& app) {
    State& st = St();
    if (st.actionsRevision == modules::ModuleUiRevision()) return;
    st.actionsRevision = modules::ModuleUiRevision();
    for (const std::string& id : st.registered) app.actions.Remove(id);
    st.registered.clear();
    for (const modules::ModuleAction& m : modules::ModuleActions()) {
        Action a;
        a.id      = ModuleActionId(m.module, m.id);
        a.label   = m.label;   // Tr() gives an unknown text back as it is
        a.icon    = m.icon;
        a.tooltip = m.tooltip;
        a.enabled = [module = m.module, id = m.id] {
            const modules::ModuleAction* now = FindAction(module, id);
            return now && now->enabled;
        };
        a.run = [&app, module = m.module, id = m.id] {
            if (!app.modules) return;
            app.modules->Call(module, [&](const BzModuleApi& api) {
                if (BZ_HAS(&api, on_action)) api.on_action(id.c_str());
            });
        };
        st.registered.push_back(a.id);
        app.actions.Add(std::move(a));
    }
}

void ModuleMenuItems(AppContext& app, std::string_view menu) {
    for (const modules::ModuleAction& m : modules::ModuleActions()) {
        if (m.menu != menu) continue;
        const Action* a = app.actions.Find(ModuleActionId(m.module, m.id));
        if (!a) continue;
        const std::string shortcut = ActionRegistry::ChordText(a->chord);
        if (ui::MenuItem(app.ui, a->Label(), shortcut.empty() ? nullptr : shortcut.c_str(), a->icon, false, a->IsEnabled())) {
            const std::string action = a->id;
            app.Later([&app, action] { app.actions.Run(action); });
        }
    }
}

void FillModuleRibbon(AppContext& app, RibbonSink& sink, bool assembly) {
    // Groups in the order the modules asked for them; a group's commands together.
    std::vector<std::string> groups;
    const auto here = [&](const modules::ModuleAction& m) {
        return !m.ribbonGroup.empty() && (m.ribbonTab == "model" || m.ribbonTab == (assembly ? "assembly" : "part"));
    };
    for (const modules::ModuleAction& m : modules::ModuleActions())
        if (here(m) && std::find(groups.begin(), groups.end(), m.ribbonGroup) == groups.end()) groups.push_back(m.ribbonGroup);
    for (const std::string& g : groups) {
        sink.BeginGroup(g.c_str());
        for (const modules::ModuleAction& m : modules::ModuleActions()) {
            if (!here(m) || m.ribbonGroup != g) continue;
            const std::string id = ModuleActionId(m.module, m.id);
            if (app.actions.Find(id)) sink.Large(id.c_str(), m.label.c_str());
        }
        sink.EndGroup();
    }
}

void DrawModulePanels(AppContext& app) {
    SyncPanels();
    State& st = St();
    if (st.panels.empty()) return;
    PanelState& p = st.panels.front();
    if (DrawPanel(app, p)) return;
    // Closed by the user (Esc, a click beside it): gone here, and the module is told.
    SendEvent(app, p, "close");
    modules::DropPanel(p.module, p.id);
}

} // namespace bz::app
