#pragma once
// «Импорт…»: STEP and STL into a new part (kernel/Import.h does the reading).
//
// The flow, as agreed with the author (mock-ups of 09.11.2026):
//   file chosen (dialog, drop, «Открыть») → quick look (ProbeImport) →
//   a window only when there is something to decide (a mesh denser than this
//   computer should show, a very large file) → a new tab with a progress card
//   over its view and «Отменить импорт» → the part with «Импортированное тело»
//   and a note of what was healed; a broken file leaves a card saying why.
// The reading runs on a thread of its own (on its own copies: it touches no
// document), the window stays alive; closing the tab cancels it.

#include "app/AppContext.h"
#include "core/Id.h"

#include <filesystem>
#include <optional>

namespace bz::app {

struct ImportJob;     // one import under way or finished (Import.cpp)
struct ImportState;   // the question asked before an import (Import.cpp)

/// Asks for a STEP or STL file when none is given, then imports it into a new part.
void StartImport(AppContext& app, std::optional<std::filesystem::path> file = {});
/// «Обновить из файла»: reads the source of the imported feature `id` again
/// (asks for the file when it is gone) and puts the new geometry in its place.
void UpdateFromFile(AppContext& app, Session& session, ObjectId id);
/// Whether `file` is something «Импорт» reads (by its extension).
bool IsImportable(const std::filesystem::path& file);

/// Takes finished imports into their parts; called once a frame.
void TickImports(AppContext& app);
/// An import is still reading (the window should keep waking up).
bool ImportsRunning(const AppContext& app);
/// The progress or error card over the view of `session`, when it has one.
void DrawImportCard(AppContext& app, Session& session, ImVec2 viewMin, ImVec2 viewMax);
/// The window asked before a dense mesh or a large file.
void DrawImportDialog(AppContext& app);
/// How far the import of `session` is (0…1) while it reads; nullopt otherwise.
std::optional<double> ImportProgressOf(const Session& session);
/// Stops the import of `session` (its tab is closing); the result is dropped.
void CancelImport(Session& session);

/// The properties of an imported feature for the left panel (source, units,
/// bodies, «Обновить из файла»); nothing for other features.
void DrawImportProperties(AppContext& app, Session& session, ObjectId id, float width);

} // namespace bz::app
