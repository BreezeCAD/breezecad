#include "platform/OpenLinks.h"

#include <deque>
#include <mutex>

namespace bz::platform {
namespace {

struct Links {
    std::mutex              mutex;
    std::deque<std::string> waiting;
};

Links& L() {
    static Links links;
    return links;
}

} // namespace

void PushOpenedLink(std::string link) {
    const std::lock_guard lock(L().mutex);
    L().waiting.push_back(std::move(link));
}

std::optional<std::string> TakeOpenedLink() {
    const std::lock_guard lock(L().mutex);
    if (L().waiting.empty()) return std::nullopt;
    std::string link = std::move(L().waiting.front());
    L().waiting.pop_front();
    return link;
}

} // namespace bz::platform
