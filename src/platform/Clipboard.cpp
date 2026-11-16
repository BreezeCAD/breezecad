#include "platform/Clipboard.h"

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstddef>
#include <cstdint>

namespace bz::platform {

bool SetClipboardImage(const Image& image) {
    const int w = image.width, h = image.height;
    if (w <= 0 || h <= 0 || image.rgba.size() < static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4) return false;
    // CF_DIB: a BITMAPINFOHEADER with the pixels right after it (a 32-bit one
    // has no colour map). Negative height — the rows come top to bottom, as
    // in Image; the channels go R and B swapped, Windows' BGRA order.
    const std::size_t pixels = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    const HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPINFOHEADER) + pixels * 4);
    if (mem == nullptr) return false;
    auto*      head = static_cast<BITMAPINFOHEADER*>(GlobalLock(mem));
    std::byte* bits = reinterpret_cast<std::byte*>(head + 1);
    head->biSize          = sizeof(BITMAPINFOHEADER);
    head->biWidth         = w;
    head->biHeight        = -h;
    head->biPlanes        = 1;
    head->biBitCount      = 32;
    head->biCompression   = BI_RGB;
    head->biSizeImage     = static_cast<DWORD>(pixels * 4);
    head->biXPelsPerMeter = head->biYPelsPerMeter = 2835;   // 72 dpi, a screen picture
    for (std::size_t i = 0; i < pixels; ++i) {
        bits[4 * i + 0] = static_cast<std::byte>(image.rgba[4 * i + 2]);
        bits[4 * i + 1] = static_cast<std::byte>(image.rgba[4 * i + 1]);
        bits[4 * i + 2] = static_cast<std::byte>(image.rgba[4 * i + 0]);
        bits[4 * i + 3] = static_cast<std::byte>(image.rgba[4 * i + 3]);
    }
    GlobalUnlock(mem);
    if (!OpenClipboard(nullptr) || !EmptyClipboard()) {
        GlobalFree(mem);
        return false;
    }
    const bool done = SetClipboardData(CF_DIB, mem) != nullptr;   // the clipboard owns it now
    CloseClipboard();
    if (!done) GlobalFree(mem);
    return done;
}

} // namespace bz::platform

#elif defined(__linux__) && !defined(__ANDROID__)

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <miniz.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace bz::platform {
namespace {

// Xlib answers errors through one process-wide handler, and its default ends
// the program: a requestor that dies mid-paste (BadWindow) must not close
// the CAD. Ours are swallowed; everything else goes to the previous handler
// (GLFW installed its own when it opened its display).
std::atomic<Display*> gOurs;
int (*gPrev)(Display*, XErrorEvent*) = nullptr;

int RouteXError(Display* d, XErrorEvent* e) {
    if (d == gOurs.load()) return 0;
    return gPrev ? gPrev(d, e) : 0;
}

} // namespace

bool SetClipboardImage(const Image& image) {
    std::size_t pngSize = 0;
    void*     png = tdefl_write_image_to_png_file_in_memory(image.rgba.data(), image.width, image.height, 4, &pngSize);
    if (png == nullptr || pngSize == 0 || pngSize > 0x7FFFFFFF) return false;
    const auto bytes = std::make_shared<std::vector<unsigned char>>(static_cast<unsigned char*>(png),
                                                                    static_cast<unsigned char*>(png) + pngSize);
    mz_free(png);
    Display* const d = XOpenDisplay(nullptr);
    if (d == nullptr) return false;
    static std::once_flag once;
    std::call_once(once, [] { gPrev = XSetErrorHandler(RouteXError); });
    gOurs.store(d);
    const Atom clipboard = XInternAtom(d, "CLIPBOARD", False);
    const Atom pngType   = XInternAtom(d, "image/png", False);
    const Atom targets   = XInternAtom(d, "TARGETS", False);
    const Window w       = XCreateSimpleWindow(d, DefaultRootWindow(d), 0, 0, 1, 1, 0, CopyFromParent, CopyFromParent);
    XSetSelectionOwner(d, clipboard, w, CurrentTime);
    if (XGetSelectionOwner(d, clipboard) != w) {   // a manager refused: give up honestly
        XDestroyWindow(d, w);
        XCloseDisplay(d);
        return false;
    }
    // The selection is answered here until another program takes it; a
    // clipboard manager usually snapshots the picture right away. Like GLFW's
    // text, the transfer is one property write (no INCR), the thread dies
    // with the process.
    std::thread([d, w, clipboard, pngType, targets, bytes] {
        for (;;) {
            XEvent e;
            XNextEvent(d, &e);
            if (e.type == SelectionRequest) {
                const XSelectionRequestEvent& r = e.xselectionrequest;
                XEvent out{};
                out.xselection.type      = SelectionNotify;
                out.xselection.send_event = True;
                out.xselection.display   = d;
                out.xselection.requestor = r.requestor;
                out.xselection.selection = r.selection;
                out.xselection.target    = r.target;
                out.xselection.time      = r.time;
                out.xselection.property  = r.property;
                if (r.target == targets && r.property != None) {
                    const Atom list[] = {targets, pngType};
                    XChangeProperty(d, r.requestor, r.property, XA_ATOM, 32, PropModeReplace,
                                    reinterpret_cast<const unsigned char*>(list), 2);
                } else if (r.target == pngType && r.property != None) {
                    XChangeProperty(d, r.requestor, r.property, pngType, 8, PropModeReplace, bytes->data(),
                                    static_cast<int>(bytes->size()));
                } else {
                    out.xselection.property = None;   // anything else, or no property to write into
                }
                XSendEvent(d, r.requestor, False, 0, &out);
            } else if (e.type == SelectionClear) {
                break;   // the clipboard went to another program
            }
        }
        XDestroyWindow(d, w);
        XCloseDisplay(d);
        Display* expected = d;
        gOurs.compare_exchange_strong(expected, nullptr);
    }).detach();
    return true;
}

} // namespace bz::platform

#else

namespace bz::platform {

bool SetClipboardImage(const Image&) { return false; }   // not implemented on this platform

} // namespace bz::platform

#endif
