#pragma once
// The test bench's eyes and hands inside the program (tools/bench).
//
// Off unless the environment names a folder: BZ_PROBE_DIR=<dir>. Then the
// program answers requests left there as files, so a test can drive it the
// way a person does (real mouse and keys) without guessing coordinates:
//
//   request.json  {"id": 7, "op": "items"}             what is on screen: every
//                                                      button, field and tree row
//                                                      with its rectangle
//                 {"op": "state"}                      document, mode, tool, busy
//                 {"op": "project", "points": [[x,y,z]]}   model point -> pixel
//                 {"op": "sketch_project", "points": [[u,v]]}  sketch point -> pixel
//                 {"op": "run", "request": {...}}      a scripting command
//                                                      (app/scripting), to make a
//                                                      reference model and to read
//                                                      back what the hands built
//   reply.json    {"id": 7, "ok": true, ...}
//
// Files, not a socket: the program stays offline (docs/RULES.md). Nothing
// here runs, allocates or is polled when the variable is not set.

#include <filesystem>
#include <string_view>

namespace bz::app {

struct AppContext;

namespace probe {

void Enable(std::filesystem::path dir);
bool Enabled();
/// Names the item just laid out for a test: a tree row and a tab draw their
/// text themselves, so Dear ImGui knows them only as "##row". Does nothing
/// when the probe is off.
void Name(std::string_view kind, std::string_view text);
/// After ImGui::NewFrame: forget the items of the frame before.
void BeginFrame();
/// After the interface is built: the items of this frame are the screen now.
void EndFrame();
/// Once per loop turn, outside the frame: answers a waiting request.
/// Returns true when a command ran (the window then draws a frame).
bool Poll(AppContext& app);

} // namespace probe
} // namespace bz::app
