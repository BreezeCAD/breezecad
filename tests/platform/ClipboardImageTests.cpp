#include "platform/Clipboard.h"

#include <doctest.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <array>
#include <string_view>

namespace {

bool IsHostedRunner() {
    char environment[32]{};
    const DWORD size = GetEnvironmentVariableA("RUNNER_ENVIRONMENT", environment, sizeof(environment));
    return size < sizeof(environment) && std::string_view(environment, size) == "github-hosted";
}

struct ClipboardWindow {
    HWND handle = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 1, 1,
                                  nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    ~ClipboardWindow() { if (handle) DestroyWindow(handle); }
};

struct ClipboardReader {
    bool open = OpenClipboard(nullptr) != FALSE;
    ~ClipboardReader() { if (open) CloseClipboard(); }
};

struct DibView {
    HGLOBAL handle;
    const BITMAPINFOHEADER* header = static_cast<const BITMAPINFOHEADER*>(GlobalLock(handle));
    ~DibView() { if (header) GlobalUnlock(handle); }
};

} // namespace

TEST_SUITE("fast") {

// This replaces the current clipboard contents, so use only the hosted runner.
TEST_CASE("Windows image clipboard preserves colors and row order" * doctest::skip(!IsHostedRunner())) {
    const ClipboardWindow owner;
    REQUIRE(owner.handle != nullptr);
    const bz::Image image{2, 2, {255, 0, 0, 255, 0, 255, 0, 255,
                                0, 0, 255, 255, 255, 255, 0, 255}};
    REQUIRE(bz::platform::SetClipboardImage(image, reinterpret_cast<std::uintptr_t>(owner.handle)));
    CHECK(GetClipboardOwner() == owner.handle);

    const ClipboardReader reader;
    REQUIRE(reader.open);
    const auto data = static_cast<HGLOBAL>(GetClipboardData(CF_DIB));
    REQUIRE(data != nullptr);
    constexpr std::array<std::uint8_t, 16> expected{0, 0, 255, 255, 0, 255, 0, 255,
                                                     255, 0, 0, 255, 0, 255, 255, 255};
    REQUIRE(GlobalSize(data) >= sizeof(BITMAPINFOHEADER) + expected.size());
    const DibView dib{data};
    REQUIRE(dib.header != nullptr);
    REQUIRE(dib.header->biSize == sizeof(BITMAPINFOHEADER));
    CHECK(dib.header->biWidth == 2);
    CHECK(dib.header->biHeight == -2);
    CHECK(dib.header->biPlanes == 1);
    CHECK(dib.header->biBitCount == 32);
    CHECK(dib.header->biCompression == BI_RGB);
    const auto* pixels = reinterpret_cast<const std::uint8_t*>(dib.header + 1);
    REQUIRE(std::equal(expected.begin(), expected.end(), pixels));
    MESSAGE("Windows clipboard CF_DIB round-trip passed");
}

} // TEST_SUITE
