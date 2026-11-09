#pragma once
// «Продолжить на устройстве» in the program (docs/dev/CONTINUE-ON-DEVICE.md).
//
// The computer: the open part or assembly is saved and sent; a window shows
// the QR and waits — the device takes the project, later gives the changes
// back; they go in place (a file changed here too is asked about) and the
// document is reopened. Closing the window stops the server.
//
// The device: the QR scanned with the camera opens the program with its
// link (platform/OpenLinks.h), or the link is pasted into «Принять с
// компьютера». The project is taken, unpacked into «С компьютера» and
// opened; «Вернуть на компьютер» saves it, gives the changes back and
// removes the copy.

namespace bz::app {

struct AppContext;

/// The computer: the active document goes to a device.
void StartHandoff(AppContext& app);
bool CanStartHandoff(AppContext& app);

/// The device: the window to paste the computer's link into.
void OpenHandoffReceive(AppContext& app);

/// The device: a project from a computer is open here.
bool HandoffOnDevice(const AppContext& app);
/// The device: the changes back to the computer, the copy removed.
void ReturnHandoff(AppContext& app);

/// The device, while a project from a computer is open: the «Вернуть на
/// компьютер» button in the header row at `pos` (between the appearance and
/// the command search: not over the model, where it got in the way of
/// drawing). Gives the width it took, 0 when there is no such project.
float DrawHandoffReturnButton(AppContext& app, float x, float y, float height);

/// Every frame: the windows, the links the system passed, the work in progress.
void DrawHandoff(AppContext& app);

} // namespace bz::app
