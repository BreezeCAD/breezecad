#pragma once
// The host side of the C ABI (bz_module.h): the table of functions every
// module gets in init(). The read API answers from the assembly the program
// last set active; the program sets it when the active tab changes and
// clears it before that assembly goes away.

#include "modules/bz_module.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace bz::kernel {
class AssemblyReader;
}

namespace bz::modules {

/// The table passed to modules; lives as long as the program.
const BzHostApi* HostApi();

/// Assembly the read API answers about; nullptr: none open (BZ_ENODATA).
/// The reader must stay valid until the next call. Main thread only.
void SetActiveAssembly(const kernel::AssemblyReader* reader);

/// What execute() runs: a JSON request in, a JSON reply out (the program
/// sets it, see app/ModuleCommands). Unset: every request gets an error.
/// Main thread only.
using CommandHandler = std::function<std::string(std::string_view request)>;
void SetCommandHandler(CommandHandler handler);

/// Called from any thread by request_main(): wake the main loop up
/// (glfwPostEmptyEvent). Set once at start, before modules load.
void SetWakeHandler(std::function<void()> wake);

/// What config_dir() answers (UTF-8). Set once at start, before modules load.
void SetConfigDir(std::string utf8);

/// True once after a module asked for main_tick (request_main). Any thread.
bool TakeMainRequest();

// --- module windows (add_action ... close_panel) ------------------------------
// The host only keeps what modules asked for; the program shows it (see
// app/shell/ModuleWindows) and calls the module back through ModuleManager.

/// A module's item in the menus and the command search.
struct ModuleAction {
    std::string module;        ///< the module's id
    std::string id;            ///< the module's own id ("start")
    std::string label, tooltip, icon;
    std::string menu;          ///< "file" or empty
    std::string ribbonTab, ribbonGroup;   ///< empty: not on the ribbon
    bool        enabled = true;
};

/// A panel a module shows; `json` as the module sent it (checked, re-written).
struct ModulePanel {
    std::string module, id, json;
};

/// Which module is running now: what add_action and the rest are filed under.
/// ModuleManager sets it around every call into a module; empty outside them.
void SetCallingModule(std::string id);

const std::vector<ModuleAction>& ModuleActions();
const std::vector<ModulePanel>&  ModulePanels();
/// Grows whenever an item or a panel changes (the program redraws then).
std::uint64_t ModuleUiRevision();

/// The user closed a panel: it is gone (the module gets the "close" event).
void DropPanel(const std::string& module, const std::string& id);
/// A module stops (unloaded, init failed): its items and panels go.
void ForgetModule(const std::string& module);

} // namespace bz::modules
