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
    std::cout << "DIAG " << what << ": state=" << int(t->status.state) << " " << t->status.message;
    if (t->site) std::cout << " | nom=" << t->site->nominal << " len=" << t->site->length;
    std::cout << std::endl;
}
doc::ThreadFeature& Thread(doc::Document& d) {
    for (auto& f : d.Features())
        if (auto* x = dynamic_cast<doc::ThreadFeature*>(f.get())) return *x;
    throw 1;
}
} // namespace

TEST_CASE("zz diag: iPad thread on Windows") {
    std::cout << "DIAG OCCT " << OCC_VERSION_COMPLETE << std::endl;
    Try("as saved", [](doc::Document&) {});
    Try("pitch 1", [](doc::Document& d) { Thread(d).pitch.expression = "1"; });
    Try("pitch 1.5", [](doc::Document& d) { Thread(d).pitch.expression = "1.5"; });
    Try("left-handed", [](doc::Document& d) { Thread(d).leftHanded = true; });
    Try("from other end", [](doc::Document& d) { Thread(d).fromOtherEnd = true; });
    Try("length 2", [](doc::Document& d) { Thread(d).fullLength = false; Thread(d).length.expression = "2"; });
    Try("cosmetic", [](doc::Document& d) { Thread(d).cosmetic = true; });
    Try("size M8 chosen", [](doc::Document& d) { Thread(d).size = 8; });
    Try("hole simple", [](doc::Document& d) {
        for (auto& f : d.Features())
            if (auto* h = dynamic_cast<doc::HoleFeature*>(f.get())) h->type = doc::HoleType::Simple;
    });
    Try("hole through", [](doc::Document& d) {
        for (auto& f : d.Features())
            if (auto* h = dynamic_cast<doc::HoleFeature*>(f.get())) h->throughAll = true;
    });
    Try("hole depth 20", [](doc::Document& d) {
        for (auto& f : d.Features())
            if (auto* h = dynamic_cast<doc::HoleFeature*>(f.get())) h->depth.expression = "20";
    });
    FAIL_CHECK("diag: show the output in CI");   // ctest prints it only on a failure
}
