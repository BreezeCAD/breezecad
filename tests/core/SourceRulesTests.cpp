// Rules the compilers do not check on every platform: caught here, on Linux,
// before a Windows build fails on them.

#include <doctest.h>

#include <filesystem>
#include <fstream>
#include <regex>
#include <string>

// The fast set: every pull request runs it (bz_tests -ts=fast, STAB-0 step 4).
TEST_SUITE_BEGIN("fast");

namespace {

/// Lines of our sources (comments and strings stripped roughly) matching `pattern`.
std::string Offenders(const std::regex& pattern) {
    std::string out;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(std::filesystem::path(BZ_SOURCE_DIR) / "src")) {
        const auto ext = entry.path().extension();
        if (ext != ".h" && ext != ".cpp") continue;
        std::ifstream in(entry.path());
        std::string line;
        for (int n = 1; std::getline(in, line); ++n) {
            if (const auto c = line.find("//"); c != std::string::npos) line.erase(c);
            line = std::regex_replace(line, std::regex(R"("([^"\\]|\\.)*")"), "\"\"");
            if (std::regex_search(line, pattern)) out += entry.path().filename().string() + ":" + std::to_string(n) + " " + line + "\n";
        }
    }
    return out;
}

} // namespace

TEST_CASE("no identifiers that windows.h turns into macros") {
    // near, far: empty macros; min, max: function-like macros (std::min( is fine with NOMINMAX, plain min( is not).
    CHECK(Offenders(std::regex(R"(\b(near|far)\b)")) == "");
    // Win32 API names that are macros (…A / …W): a function of ours by that name breaks.
    CHECK(Offenders(std::regex(R"(\b(LoadImage|DrawText|CreateWindow|GetObject|SendMessage|CreateFont)\s*\()")) == "");
}

TEST_CASE("booleans of the geometry kernel only in kernel/occt/Ops") {
    // STAB-3: every boolean goes through one pipeline (exact → fuzzy → glued →
    // healed, checked, under a deadline). A bare BRepAlgoAPI_* elsewhere skips
    // all that and puts OCCT's words in front of the user.
    std::string offenders = Offenders(std::regex(R"(\b(BRepAlgoAPI_\w+|BOPAlgo_(Builder|BOP|Splitter|MakerVolume|Section))\b)"));
    // Allowed: the pipeline itself; projections (Project.cpp) — stage 5 of the plan, not touched yet.
    std::string kept;
    for (std::size_t at = 0, next; at < offenders.size(); at = next + 1) {
        next = offenders.find('\n', at);
        if (next == std::string::npos) next = offenders.size();
        const std::string line = offenders.substr(at, next - at);
        if (line.starts_with("Ops.cpp:") || line.starts_with("Ops.h:") || line.starts_with("Project.cpp:")) continue;
        kept += line + "\n";
    }
    CHECK(kept == "");
}

TEST_SUITE_END();
