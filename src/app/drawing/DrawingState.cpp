#include "app/drawing/DrawingState.h"

#include "core/PathText.h"

#include <algorithm>
#include <utility>

namespace bz::app {

namespace {
constexpr std::size_t kUndoDepth = 200;   // snapshots of a drawing are small: kilobytes
}

drawing::Sheet& DrawingState::Sheet() {
    for (drawing::Sheet& s : drawing.sheets)
        if (s.id == activeSheet) return s;
    if (drawing.sheets.empty()) {
        drawing::Sheet first;
        first.id   = drawing.Ids().Next();
        first.name = "Лист 1";
        drawing.sheets.push_back(std::move(first));
    }
    activeSheet = drawing.sheets.front().id;
    return drawing.sheets.front();
}

const drawing::Sheet& DrawingState::Sheet() const { return const_cast<DrawingState*>(this)->Sheet(); }

int DrawingState::SheetNumber() const {
    for (std::size_t i = 0; i < drawing.sheets.size(); ++i)
        if (drawing.sheets[i].id == Sheet().id) return static_cast<int>(i) + 1;
    return 1;
}

void DrawingState::Change(std::string label, const std::function<void(drawing::Drawing&)>& change) {
    myUndo.push_back({std::move(label), drawing::ToJson(drawing)});
    if (myUndo.size() > kUndoDepth) myUndo.erase(myUndo.begin());
    myRedo.clear();
    change(drawing);
    ++myRevision;
}

void DrawingState::Restore(const std::string& json) {
    Expected<drawing::Drawing> back = drawing::FromJson(json);
    if (!back) return;   // our own snapshot: cannot fail; if it does, nothing changes
    const ObjectId shown = activeSheet;
    drawing = std::move(*back);
    activeSheet = shown;   // Sheet() falls back to the first one when it is gone
    ++myRevision;
}

void DrawingState::Undo() {
    if (myUndo.empty()) return;
    Step step = std::move(myUndo.back());
    myUndo.pop_back();
    myRedo.push_back({step.label, drawing::ToJson(drawing)});
    Restore(step.json);
}

void DrawingState::Redo() {
    if (myRedo.empty()) return;
    Step step = std::move(myRedo.back());
    myRedo.pop_back();
    myUndo.push_back({step.label, drawing::ToJson(drawing)});
    Restore(step.json);
}

std::string DrawingState::StoredPath(const std::filesystem::path& path) {
    const std::u8string text = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

void DrawingState::RebasePaths(drawing::Drawing& d, const std::filesystem::path& from, const std::filesystem::path& target) {
    const std::filesystem::path oldDir = from.empty() ? std::filesystem::path{} : from.parent_path();
    const std::filesystem::path newDir = target.parent_path();
    for (drawing::ModelRef& m : d.models) {
        const std::filesystem::path p = PathFromUtf8(m.path);
        // Before the first save a model is kept by its full path; after it, relative to the drawing.
        const std::filesystem::path full = p.is_absolute() || oldDir.empty() ? p : (oldDir / p).lexically_normal();
        if (!full.is_absolute()) continue;   // relative with nothing to anchor it: left as it is
        std::filesystem::path relative = full.lexically_relative(newDir);
        if (relative.empty()) relative = full;   // another drive: only the full path works
        m.path = StoredPath(relative);
    }
}

} // namespace bz::app
