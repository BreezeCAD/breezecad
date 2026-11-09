#pragma once
// Links the system hands to the program: a "breezecad://handoff?…" QR code
// scanned with the phone's camera opens BreezeCAD with it (Android: an
// intent-filter in the manifest, BreezeActivity passes the link on; iOS:
// the app delegate's openURL calls PushOpenedLink). On a computer the link
// is pasted into «Принять с компьютера» instead.
#include <optional>
#include <string>

namespace bz::platform {

/// Any thread: a link the system gave the program.
void PushOpenedLink(std::string link);
/// The UI thread: the oldest link not yet taken.
std::optional<std::string> TakeOpenedLink();

} // namespace bz::platform
