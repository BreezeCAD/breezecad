#include "app/features/PhotoEditor.h"

#include "app/features/OperationEditors.h"
#include "app/features/ToolChoice.h"
#include "app/Session.h"
#include "app/sketch/SketchEditor.h"
#include "core/PathText.h"
#include "platform/FileDialogs.h"
#include "platform/Paths.h"
#include "render/Viewport.h"
#include "ui/Widgets.h"

#include <imgui.h>
#include <miniz.h>

#include <array>
#include <cstdlib>
#include <fstream>

namespace bz::app {
namespace {

struct Size {
    int         width, height;
    const char* label;
};
constexpr std::array<Size, 4> kSizes{{{1280, 720, "1280×720"}, {1920, 1080, "1920×1080"}, {3840, 2160, "3840×2160"}, {1080, 1080, "1080×1080"}}};

void Caption(AppContext& app, const char* text, float width) {
    ui::FontScope font(app.ui.fonts.caption);
    ImGui::PushStyleColor(ImGuiCol_Text, ui::ToVec4(app.ui.C().textMuted));
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

} // namespace

void PhotoEditor::Start(AppContext& app, Session& session) {
    if (session.model.Bodies().empty()) {
        app.notes.Info(Tr("Сначала создайте тело: в кадре нечего показать"));
        return;
    }
    app.ops.Close(app, session);
    if (session.mode == EditMode::Sketch) app.sketch.Finish(app, session);
    myOpen = true;
    myOwner = &session;
}

void PhotoEditor::Cancel(AppContext& app, Session& session) {
    (void)app;
    (void)session;
    myOpen = false;
}

double PhotoEditor::Aspect() const {
    const Size& s = kSizes[static_cast<std::size_t>(mySize)];
    return static_cast<double>(s.width) / s.height;
}

std::string PhotoEditor::Hint() const {
    return Tr("Поверните деталь, как она должна выглядеть на картинке, и нажмите «Сохранить PNG»");
}

void PhotoEditor::Save(AppContext& app, Session& session) {
    const Size& size = kSizes[static_cast<std::size_t>(mySize)];
    render::Viewport::PhotoOptions o;
    o.background = static_cast<render::Viewport::PhotoOptions::Background>(myBackground);
    o.width = size.width;
    o.height = size.height;
    o.floor = myFloor;
    o.edges = myEdges;
    const std::optional<Image> image = app.viewport.Photo(o);
    if (!image) {
        app.notes.Error(Tr("Не удалось сделать кадр: видеокарта не отдала изображение"));
        return;
    }
    std::size_t bytes = 0;
    void* png = tdefl_write_image_to_png_file_in_memory(image->rgba.data(), image->width, image->height, 4, &bytes);
    if (!png) {
        app.notes.Error(Tr("Не удалось сжать кадр в PNG"));
        return;
    }
    std::error_code ec;
    std::filesystem::path folder = myFolder;
    if (folder.empty() || !std::filesystem::is_directory(folder, ec)) folder = app.settings.lastSaveFolder;
    if (folder.empty() || !std::filesystem::is_directory(folder, ec)) folder = platform::DocumentsDir();
    // Test runs (Xvfb, no file window): BZ_PHOTO_FILE names the file.
    const char* testFile = std::getenv("BZ_PHOTO_FILE");
    const auto file = testFile ? std::optional<std::filesystem::path>(PathFromUtf8(testFile)) : platform::ShowSaveDialog(Tr("Сохранить кадр"), folder / PathFromUtf8(session.Title() + ".png"), {{Tr("Картинка PNG"), "*.png"}},
                                               ".png");
    if (!file) {
        mz_free(png);
        return;
    }
    bool written = false;
    {
        std::ofstream out(*file, std::ios::binary);
        out.write(static_cast<const char*>(png), static_cast<std::streamsize>(bytes));
        written = static_cast<bool>(out);
    }
    mz_free(png);
    if (!written || !platform::FileWritten(*file)) {
        app.notes.Error(Trf("Не удалось записать {}", ToUtf8(file->filename())));
        return;
    }
    myFolder = file->parent_path();
    app.notes.Info(Trf("Кадр сохранён: {}", ToUtf8(file->filename())));
}

void PhotoEditor::DrawInViewport(AppContext& app, Session& session, const ViewportFrame& vf) {
    (void)session;
    // What the picture holds: the rest of the view dimmed, the frame outlined.
    const auto f = app.viewport.PhotoFrame(Aspect());
    const ImVec2 a{vf.min.x + static_cast<float>(f[0]), vf.min.y + static_cast<float>(f[1])};
    const ImVec2 b{a.x + static_cast<float>(f[2]), a.y + static_cast<float>(f[3])};
    const ui::ThemeColors& c = app.ui.C();
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    Color veil = c.appBackground;
    veil.a = 0.6f;
    const ImU32 v = ui::ToU32(veil);
    dl->PushClipRect(vf.min, vf.max, true);
    dl->AddRectFilled(vf.min, {vf.max.x, a.y}, v);
    dl->AddRectFilled({vf.min.x, b.y}, vf.max, v);
    dl->AddRectFilled({vf.min.x, a.y}, {a.x, b.y}, v);
    dl->AddRectFilled({b.x, a.y}, {vf.max.x, b.y}, v);
    dl->AddRect(a, b, ui::ToU32(c.accent), 0, 0, app.ui.S(1.5f));
    dl->PopClipRect();
}

void PhotoEditor::DrawPanel(AppContext& app, Session& session) {
    ui::Ui& ui = app.ui;
    const ui::ThemeColors& c = ui.C();
    const float width = ImGui::GetContentRegionAvail().x;
    const bool phone = app.shell.mobile.phone;

    if (!phone) {   // header: icon, title, close
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float h = ui.S(48);
        ui.icons.Draw(dl, "view-render", {p.x + ui.S(14), p.y + (h - ui.S(24)) / 2}, ui.S(24), {1, 1, 1, 1});
        ui::FittedText(dl, ui.fonts.title, ui.S(14), {p.x + ui.S(48), p.y + (h - ui.S(14)) / 2 - ui.S(1)}, p.x + width - ui.S(40),
                       ui::ToU32(c.text), Title());
        ImGui::SetCursorScreenPos({p.x + width - ui.S(38), p.y + ui.S(9)});
        if (ui::IconButton(ui, "##close-photo", "ui-close", 30, 14, Tr("Закрыть (Esc)"))) app.Later([&app, &session, this] { Cancel(app, session); });
        ImGui::SetCursorScreenPos({p.x, p.y + h});
        ui::Divider(ui, c.dividerSoft);
    }

    const float footer = phone ? 0.0f : ui.S(62);
    ImGui::BeginChild("##photo-body", {width, ImGui::GetContentRegionAvail().y - footer}, ImGuiChildFlags_None);
    ImGui::SetCursorPos({ui.S(14), ui.S(14)});
    ImGui::BeginGroup();
    const float inner = width - ui.S(28);

    static constexpr std::array<ChoiceItem, 3> backgrounds{{{nullptr, N_("Студия")}, {nullptr, N_("Тёмный")}, {nullptr, N_("Прозрачный")}}};
    myBackground = ToolChoice(app, "photo-bg", N_("Фон"), backgrounds, myBackground, inner);
    const bool transparent = myBackground == 2;
    static constexpr std::array<ChoiceItem, 2> yesNo{{{nullptr, N_("Да")}, {nullptr, N_("Нет")}}};
    const int floor = ToolChoice(app, "photo-floor", N_("Пол и тень"), yesNo, transparent ? 1 : myFloor ? 0 : 1, inner, transparent ? 1 : 0);
    if (!transparent) myFloor = floor == 0;
    myEdges = ToolChoice(app, "photo-edges", N_("Рёбра"), yesNo, myEdges ? 0 : 1, inner) == 0;
    static constexpr std::array<ChoiceItem, 4> sizes{{{nullptr, kSizes[0].label}, {nullptr, kSizes[1].label}, {nullptr, kSizes[2].label},
                                                      {nullptr, kSizes[3].label}}};
    mySize = ToolChoice(app, "photo-size", N_("Размер картинки"), sizes, mySize, inner);

    ImGui::Dummy({0, ui.S(6)});
    Caption(app,
            transparent ? Tr("Прозрачный фон — PNG с прозрачностью: деталь ложится на любой фон поста или сайта. Пол и тень здесь не рисуются.")
                        : Tr("Камера — как в окне: рамка показывает, что попадёт в картинку."),
            inner);
    ImGui::EndGroup();
    ImGui::EndChild();
    if (phone) return;

    ui::Divider(ui, c.dividerSoft);
    const ImVec2 fp = ImGui::GetCursorScreenPos();
    const float gap = ui.S(8);
    const float bw = (width - ui.S(28) - gap) / 2.0f;
    const float bh = ui.S(36);
    ImGui::SetCursorScreenPos({fp.x + ui.S(14), fp.y + ui.S(12)});
    if (ui::SecondaryButton(ui, "##photo-close", Tr("Закрыть"), nullptr, {bw, bh})) app.Later([&app, &session, this] { Cancel(app, session); });
    ImGui::SameLine(0, gap);
    if (ui::PrimaryButton(ui, "##photo-save", Tr("Сохранить PNG…"), nullptr, {bw, bh})) app.Later([&app, &session, this] { Save(app, session); });
}

} // namespace bz::app
