#pragma once
// Monitoring for test runs (tools/monitor): what the user did, how long
// frames and rebuilds took, hangs, memory, warnings and crashes, as one
// JSON line per event in a file named by the BZ_MONITOR variable. Without
// it everything here costs a branch, except Action(), which also writes one
// debug line to the log (user actions are rare; the log is local).

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "core/Profile.h"

namespace bz::mon {

/// Reads BZ_MONITOR / BZ_MONITOR_HANG_MS and starts writing (and the hang
/// watchdog) if set. Call once, early in main.
void StartFromEnvironment();
/// Writes the end of the session and stops the watchdog.
void Stop(int exitCode);
bool Enabled();

/// A user action or command: `source` says where it came from ("action",
/// "command", "assembly", "open", "undo"...), `name` what it was.
void Action(std::string_view source, std::string_view name);
/// The last action, for crash and hang reports ("" if none).
std::string_view LastAction();
/// What exactly that action was asked (a command's arguments): a line of
/// its own in a hang report, not part of the action (and of the error code).
void ActionDetail(std::string_view detail);
std::string LastActionDetail();

/// The window's loop is alive (called every loop turn and frame). Always
/// kept, monitoring or not: the hang window (app/CrashGuard) watches it.
void Beat();
/// Seconds since the last Beat; 0 while the loop waits on purpose (Waiting).
double SecondsSinceBeat();
/// One frame drawn in `ms`; `parts` from prof::TakeFrame.
void Frame(double ms, const std::vector<prof::Part>& parts);
/// Resident memory, MB (sent when it changes noticeably).
void Memory(double mb);
/// A finished piece of work worth timing: a rebuild, loading parts...
void Timed(std::string_view kind, std::string_view detail, double ms);
/// Frames drawn over the last `seconds` and why (Application::ReportRedraws).
void Redraws(int frames, double seconds, std::string_view why);
/// A warning or an error from the log.
void LogLine(bool error, std::string_view message);
/// From a crash handler: best effort, no allocation, no locks.
void CrashNote(const char* text);
/// Folder for crash dumps (next to the monitor file), empty if disabled.
const std::filesystem::path& DumpDir();

/// A known long piece of work (a rebuild, an export, opening a file): what
/// it is, which step it is at and how many there are. Kept monitoring or
/// not, from any thread: while its steps go on, the window's loop standing
/// still is work, not a hang (app/CrashGuard), and a hang report names it.
/// `what` and `step` are shown to the user as they are («Перестроение»,
/// the operation's name); `total` 0 — the number of steps is unknown.
void Work(std::string_view what, std::string_view step, std::size_t done, std::size_t total);
/// That work is over.
void WorkDone();
struct WorkState {
    bool        active = false;
    std::string what, step;
    std::size_t done = 0, total = 0;
    double      secondsSinceStep = 0;   ///< since `step` or `done` last changed
};
WorkState CurrentWork();
/// "Перестроение: «Вырез 14», операция 31 из 52" — or "" when nothing is under way.
std::string DescribeWork(const WorkState& work);

/// The loop blocks on purpose (a native file dialog): not a hang.
class Waiting {
public:
    Waiting();
    ~Waiting();
    Waiting(const Waiting&) = delete;
    Waiting& operator=(const Waiting&) = delete;
};

} // namespace bz::mon
