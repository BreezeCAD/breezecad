#pragma once
// The last tools of the heavy operations (a coil, a thread), by a key made
// of everything the tool depends on: a rebuild that reaches the operation
// with the same inputs (an edit above it that did not touch its sketch or
// sizes, undo and redo, the background rebuild after a preview) takes the
// tool from here instead of sweeping it again (BETA-BUGS №7). Shapes are
// shared, never changed: booleans make new ones.

#include <TopoDS_Shape.hxx>

#include <cstdio>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace bz::kernel::occt {

class ToolCache {
public:
    explicit ToolCache(std::size_t size) : mySize(size) {}

    std::optional<TopoDS_Shape> Find(const std::string& key) {
        const std::lock_guard lock(myMutex);
        for (const auto& [k, shape] : myItems)
            if (k == key) return shape;
        return std::nullopt;
    }

    void Put(std::string key, const TopoDS_Shape& shape) {
        const std::lock_guard lock(myMutex);
        myItems.emplace_front(std::move(key), shape);
        while (myItems.size() > mySize) myItems.pop_back();
    }

    void Clear() {
        const std::lock_guard lock(myMutex);
        myItems.clear();
    }

private:
    std::size_t                                       mySize;
    std::mutex                                        myMutex;
    std::deque<std::pair<std::string, TopoDS_Shape>> myItems;
};

/// Appends numbers to a cache key exactly enough (17 significant digits).
inline void AddToKey(std::string& key, double v) {
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%.17g;", v);
    key += buffer;
}

} // namespace bz::kernel::occt
