#pragma once
// What modules put into the program's interface (bz_module.h, BZ_MODULE_PANELS):
// their commands in the File menu, the command search and the ribbon, and
// the panels they show. The host keeps what the modules asked for
// (modules/ModuleHost.h); this draws it and calls the module back.
//
// The modules' texts are shown as they come: a module translates its own.

#include <string>
#include <string_view>

namespace bz::app {

struct AppContext;
struct ViewportFrame;
class RibbonSink;
class Session;

/// The id a module's command has among the program's actions ("module.conn.start").
std::string ModuleActionId(std::string_view module, std::string_view id);

/// Brings the modules' commands into app.actions when a module changed them.
/// Every frame, before the menus; cheap when nothing changed.
void SyncModuleActions(AppContext& app);

/// The modules' commands for a menu ("file").
void ModuleMenuItems(AppContext& app, std::string_view menu);

/// The modules' groups at the end of the «Модель» tab. A command asks for
/// "ribbon": {"tab": "model"} (a part and an assembly), "part" or "assembly".
void FillModuleRibbon(AppContext& app, RibbonSink& sink, bool assembly);

/// The panel a module shows (one at a time: the first one asked for).
/// `assembly_viewport: true` makes it nonmodal and ties it to the active assembly.
/// `number` items report doubles in `values`; `polyline_xyz_mm` is a list of XYZ triples.
void DrawModulePanels(AppContext& app);
void DrawModuleAssemblyPreview(AppContext& app, const Session& session, const ViewportFrame& vf);

} // namespace bz::app
