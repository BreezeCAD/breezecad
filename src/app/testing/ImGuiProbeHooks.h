#pragma once
// Dear ImGui's item hooks (IMGUI_ENABLE_TEST_ENGINE), passed on to whoever
// listens. Compiled into bz_imgui only with -DBZ_UI_PROBE=ON, so every
// program linking Dear ImGui (the tests, the installer) still links; the
// listener is app/testing/UiProbe.cpp, and only under BZ_PROBE_DIR.

#include <imgui.h>
#include <imgui_internal.h>

namespace bz::probe_hooks {

using ItemAddFn = void (*)(ImGuiContext*, ImGuiID, const ImRect&, const ImGuiLastItemData*);
using ItemInfoFn = void (*)(ImGuiContext*, ImGuiID, const char*, ImGuiItemStatusFlags);

extern ItemAddFn  itemAdd;    ///< null: nobody listens
extern ItemInfoFn itemInfo;

} // namespace bz::probe_hooks
