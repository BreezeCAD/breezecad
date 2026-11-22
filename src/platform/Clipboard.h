#pragma once
// A picture in the system clipboard: the «Кадр» panel's «Копировать».
// Desktop only — Windows takes a DIB (CF_DIB), X11 — image/png, served by a
// thread of its own for as long as this program owns the selection. Where it
// is not implemented (macOS, the phones) or it failed: false, the caller
// tells the user so.

#include "core/Image.h"

#include <cstdint>

namespace bz::platform {

/// The picture in the system clipboard. The X11 selection answers while this
/// program runs; with a clipboard manager (the usual desktop) the picture
/// outlives it, like the text GLFW puts there.
/// On Windows, nativeWindow is the HWND that takes ownership of the clipboard.
bool SetClipboardImage(const Image& image, std::uintptr_t nativeWindow);

} // namespace bz::platform
