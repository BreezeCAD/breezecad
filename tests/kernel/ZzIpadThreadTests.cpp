// TEMPORARY diagnosis (branch diag/thread-windows, not for merging): the
// thread from the iPad file that faults inside OCCT on Windows only.
#include "document/BzcFile.h"
#include "document/HoleFeature.h"
#include "document/ThreadFeature.h"
#include "kernel/Model.h"
#include <Standard_Version.hxx>
#include <doctest.h>
#include <filesystem>
#include <functional>
#include <iostream>
using namespace bz;

namespace {
void Try(const char* what, const std::function<void(doc::Document&)>& change) {
    auto d = doc::LoadBzc(std::filesystem::path(__FILE__).parent_path().parent_path() / "data" / "thread-ipad.bzc");
    REQUIRE(d);
    doc::ThreadFeature* t = nullptr;
    for (auto& f : d->Features())
        if (auto* x = dynamic_cast<doc::ThreadFeature*>(f.get())) t = x;
    REQUIRE(t);
    change(*d);
    kernel::Model model;
    model.Rebuild(*d);
    std::cout << "DIAG " << what << ": state=" << int(t->status.state) << " vol=" << model.Volume() << " " << t->status.message;
    if (t->site) std::cout << " | nom=" << t->site->nominal << " len=" << t->site->length;
    std::cout << std::endl;
}
doc::ThreadFeature& Thread(doc::Document& d) {
    for (auto& f : d.Features())
        if (auto* x = dynamic_cast<doc::ThreadFeature*>(f.get())) return *x;
    throw 1;
}
} // namespace

void SetMode(const char* m) {
#if defined(_WIN32)
    _putenv_s("BZ_THREAD_CLASSIFY", m);
#else
    setenv("BZ_THREAD_CLASSIFY", m, 1);
#endif
}

TEST_CASE("zz diag: iPad thread on Windows") {
    std::cout << "DIAG OCCT " << OCC_VERSION_COMPLETE << std::endl;
    for (const char* m : {"D", "B", "C", "E", "F", "A"}) {
        SetMode(m);
        const std::string as = std::string("mode ") + m + " saved";
        Try(as.c_str(), [](doc::Document&) {});
        const std::string cos = std::string("mode ") + m + " cosmetic";
        Try(cos.c_str(), [](doc::Document& d) { Thread(d).cosmetic = true; });
        const std::string other = std::string("mode ") + m + " other end";
        Try(other.c_str(), [](doc::Document& d) { Thread(d).fromOtherEnd = true; });
    }
    FAIL_CHECK("diag: show the output in CI");   // ctest prints it only on a failure
}
