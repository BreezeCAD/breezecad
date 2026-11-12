#include "ImGuiProbeHooks.h"

namespace bz::probe_hooks {
ItemAddFn  itemAdd = nullptr;
ItemInfoFn itemInfo = nullptr;
} // namespace bz::probe_hooks

void ImGuiTestEngineHook_ItemAdd(ImGuiContext* ctx, ImGuiID id, const ImRect& bb, const ImGuiLastItemData* data) {
    if (bz::probe_hooks::itemAdd) bz::probe_hooks::itemAdd(ctx, id, bb, data);
}

void ImGuiTestEngineHook_ItemInfo(ImGuiContext* ctx, ImGuiID id, const char* label, ImGuiItemStatusFlags flags) {
    if (bz::probe_hooks::itemInfo) bz::probe_hooks::itemInfo(ctx, id, label, flags);
}

void ImGuiTestEngineHook_Log(ImGuiContext*, const char*, ...) {}

const char* ImGuiTestEngine_FindItemDebugLabel(ImGuiContext*, ImGuiID) { return nullptr; }
