#include "../Fixtures.h"
#include "document/BzcFile.h"
#include "document/ThreadFeature.h"
#include "kernel/Model.h"
#include "kernel/occt/ModelImpl.h"
#include "kernel/occt/Helix.h"
#include "kernel/occt/Ops.h"

#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <chrono>

#include <doctest.h>

#include <filesystem>

// The fast set: every pull request runs it (bz_tests -ts=fast, STAB-0 step 4).
TEST_SUITE_BEGIN("fast");

using namespace bz;
using namespace bz::test;
using doc::FeatureState;

namespace {

doc::ThreadFeature& AddThread(doc::Document& d, double radius, const std::string& pitch) {
    auto t = std::make_unique<doc::ThreadFeature>();
    t->id = d.Ids().Next();
    t->name = d.UniqueFeatureName("Резьба");
    t->face = doc::CylinderTouch{{0, 0, 0}, {0, 0, 1}, radius, {1, 0, 0}};
    t->pitch.expression = pitch;
    auto* raw = t.get();
    d.Insert(std::move(t), d.FeatureCount());
    return *raw;
}

} // namespace

TEST_CASE("an external thread cuts a groove along the shaft") {
    doc::Document d;
    auto& sk = AddSketch(d);
    AddCircle(d, sk, {0, 0}, 4);
    AddExtrude(d, sk.id, "20");
    auto& thread = AddThread(d, 4, "1.25");
    kernel::Model model;
    model.Rebuild(d);
    REQUIRE(thread.status.state == FeatureState::Ok);
    const double shaft = kPi * 16 * 20;
    // Groove: a 60° triangle 0.6134P deep, about 0.22 P² in section, 16 turns.
    const double cut = shaft - model.Volume();
    CHECK(cut > 80);
    CHECK(cut < 180);

    thread.fullLength = false;
    thread.length.expression = "10";
    d.MarkChanged(2);
    model.Rebuild(d);
    REQUIRE(thread.status.state == FeatureState::Ok);
    CHECK(shaft - model.Volume() == doctest::Approx(cut / 2).epsilon(0.15));
}

TEST_CASE("an internal thread cuts into the hole wall") {
    doc::Document d;
    auto& sk = AddSketch(d);
    AddRect(d, sk, {-10, -10}, {10, 10});
    AddCircle(d, sk, {0, 0}, 3.4);
    AddExtrude(d, sk.id, "12");
    auto& thread = AddThread(d, 3.4, "1.25");
    thread.leftHanded = true;
    kernel::Model model;
    model.Rebuild(d);
    REQUIRE(thread.status.state == FeatureState::Ok);
    const double plate = (400 - kPi * 3.4 * 3.4) * 12;
    CHECK(plate - model.Volume() > 30);
    CHECK(plate - model.Volume() < 150);
    CHECK(doc::CoarsePitch(8) == 1.25);
    CHECK(doc::CoarsePitch(6.1) == 1.0);
    REQUIRE(thread.site);
    CHECK_FALSE(thread.site->external);
    CHECK(thread.site->nominal == 8);
    CHECK(doc::StandardSize(7.97) == 8);
    CHECK(doc::StandardSize(7) == 7);
}

TEST_CASE("a thread without a cylinder reports it") {
    doc::Document d;
    auto& sk = AddSketch(d);
    AddRect(d, sk, {-10, -10}, {10, 10});
    AddExtrude(d, sk.id, "12");
    auto& thread = AddThread(d, 4, "1.25");
    kernel::Model model;
    model.Rebuild(d);
    CHECK(thread.status.state == FeatureState::Error);
}

TEST_CASE("a thread survives saving and loading") {
    doc::Document d;
    auto& sk = AddSketch(d);
    AddCircle(d, sk, {0, 0}, 4);
    AddExtrude(d, sk.id, "20");
    auto& thread = AddThread(d, 4, "1.25");
    thread.fullLength = false;
    thread.fromOtherEnd = true;
    thread.leftHanded = true;
    thread.length.expression = "7";
    const auto loaded = doc::DocumentFromJson(doc::DocumentToJson(d));
    REQUIRE(loaded);
    const auto* t = dynamic_cast<const doc::ThreadFeature*>(loaded->Features()[2].get());
    REQUIRE(t);
    REQUIRE(t->face);
    CHECK(t->face->radius == 4);
    CHECK_FALSE(t->fullLength);
    CHECK(t->fromOtherEnd);
    CHECK(t->leftHanded);
    CHECK(t->length.expression == "7");
}

TEST_CASE("a thread stops short of a shoulder and runs out of a free end") {
    const auto cutOf = [](bool onPlate) {
        doc::Document d;
        if (onPlate) {
            auto& plate = AddSketch(d);
            AddRect(d, plate, {-10, -10}, {10, 10});
            AddExtrude(d, plate.id, "5");
        }
        auto& sk = AddSketch(d);
        AddCircle(d, sk, {0, 0}, 4);
        AddExtrude(d, sk.id, onPlate ? "25" : "20", onPlate ? doc::BooleanOp::Join : doc::BooleanOp::NewBody);
        kernel::Model before;
        before.Rebuild(d);
        auto& thread = AddThread(d, 4, "1.25");
        kernel::Model model;
        model.Rebuild(d);
        REQUIRE(thread.status.state == FeatureState::Ok);
        return before.Volume() - model.Volume();
    };
    CHECK(cutOf(true) < cutOf(false) - 1);
}

TEST_CASE("a chosen standard size wins over the shaft, a cosmetic thread cuts nothing") {
    // An 8.6 mm shaft with M8 chosen: cut down to 8 mm, then threaded, so
    // it ends up the same as an M8 thread on an 8 mm shaft.
    const auto threaded = [](double radius, double size, bool cosmetic) {
        doc::Document d;
        auto& sk = AddSketch(d);
        AddCircle(d, sk, {0, 0}, radius);
        AddExtrude(d, sk.id, "20");
        auto& thread = AddThread(d, radius, "1.25");
        thread.size = size;
        thread.cosmetic = cosmetic;
        kernel::Model model;
        model.Rebuild(d);
        REQUIRE(thread.status.state == FeatureState::Ok);
        return model.Volume();
    };
    const double m8 = threaded(4, 0, false);
    CHECK(threaded(4.3, 8, false) == doctest::Approx(m8).epsilon(0.01));
    CHECK(threaded(4, 0, true) == doctest::Approx(kPi * 16 * 20).epsilon(1e-6));

    doc::Document d;
    auto& sk = AddSketch(d);
    AddCircle(d, sk, {0, 0}, 4);
    AddExtrude(d, sk.id, "20");
    auto& thread = AddThread(d, 4, "1.25");
    thread.size = 8;
    thread.cosmetic = true;
    const auto loaded = doc::DocumentFromJson(doc::DocumentToJson(d));
    REQUIRE(loaded);
    const auto* t = dynamic_cast<const doc::ThreadFeature*>(loaded->Features()[2].get());
    REQUIRE(t);
    CHECK(t->size == 8);
    CHECK(t->cosmetic);
    CHECK(doc::StandardSizes().size() == 22);
}

TEST_CASE("an internal thread in a hole under a counterbore builds (made on an iPad)") {
    // The part from the iPad: a blind Ø6.6 hole under a Ø11 counterbore, an
    // M8 thread in it. On Windows (OCCT 7.9.3 of MSYS2) a classifier loaded
    // with the body and asked twice faulted inside OCCT, the thread got
    // "сбой внутри OpenCASCADE"; Linux and macOS built it.
    Expected<doc::Document> d = doc::LoadBzc(std::filesystem::path(BZ_SOURCE_DIR) / "tests" / "data" / "thread-counterbore.bzc");
    REQUIRE(d);
    kernel::Model model;
    model.Rebuild(*d);
    const doc::ThreadFeature* thread = nullptr;
    for (const auto& f : d->Features())
        if (const auto* t = dynamic_cast<const doc::ThreadFeature*>(f.get())) thread = t;
    REQUIRE(thread);
    REQUIRE_MESSAGE(thread->status.state == FeatureState::Ok, thread->status.message);
    REQUIRE(thread->site);
    CHECK(thread->site->nominal == doctest::Approx(8));
    CHECK(thread->site->length == doctest::Approx(3.6));
    // The same on every system: Linux 8.0.1 / 7.6.3, macOS and Windows 7.9.3.
    CHECK(model.Volume() == doctest::Approx(14789.1).epsilon(1e-4));
}

TEST_CASE("a thread whose web is thinner than 0.05 mm warns") {
    doc::Document d;
    auto& sk = AddSketch(d);
    AddCircle(d, sk, {0, 0}, 2);
    AddExtrude(d, sk.id, "3");
    auto& thread = AddThread(d, 2, "0.15");   // web 0.29 P = 0.044 mm
    kernel::Model model;
    model.Rebuild(d);
    CHECK(thread.status.state == FeatureState::Warning);
    CHECK(thread.status.message.find("Перемычка между витками") == 0);
    thread.pitch.expression = "0.5";
    d.MarkChanged(2);
    model.Rebuild(d);
    CHECK(thread.status.state == FeatureState::Ok);
}

TEST_SUITE_END();

// The web of a thread between two turns of its groove, 0.02 mm (pitch 0.07:
// the groove is 0.708 P wide at the surface): what each touching tolerance
// leaves of it — the numbers behind the largest of kFuzzySteps.
// Run: bz_tests -ts=measure -s
TEST_CASE("measure: touching tolerance against a thread's 0.02 mm web" * doctest::test_suite("measure")) {
    doc::Document d;
    auto& sk = AddSketch(d);
    AddCircle(d, sk, {0, 0}, 2);
    AddExtrude(d, sk.id, "2");
    auto& thread = AddThread(d, 2, "0.07");
    kernel::Model model;
    thread.suppressed = true;
    model.Rebuild(d);
    const TopoDS_Shape body = model.GetImpl().bodies.front().shape;
    thread.suppressed = false;
    d.MarkChanged(2);
    model.Rebuild(d);
    REQUIRE(thread.status.state != FeatureState::Error);
    const TopoDS_Shape tool = model.GetImpl().tools.at(thread.id).shape;
    const auto volume = [](const TopoDS_Shape& s) {
        GProp_GProps p;
        BRepGProp::VolumeProperties(s, p);
        return p.Mass();
    };
    double exact = 0;
    for (double fuzzy : {0.0, 1e-5, 1e-4, 1e-3, 5e-3, 1e-2}) {
        kernel::occt::ops::Options o;
        o.exactFirst = fuzzy == 0.0;
        o.retry = fuzzy > 0.0;
        o.fuzzySteps = {fuzzy};
        o.glue = false;
        const auto t = std::chrono::steady_clock::now();
        const auto r = kernel::occt::ops::Boolean(kernel::occt::ops::Kind::Cut, body, tool, o);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
        const double left = r.Usable() ? volume(r.shape) : 0.0;
        if (fuzzy == 0.0) exact = left;
        MESSAGE(std::string("thread web 0.02, fuzzy ") << fuzzy << ": code " << static_cast<int>(r.code) << ", volume left " << left
                                                       << " mm3 (exact " << exact << "), " << ms << " ms");
    }
}

// How the helix is cut into edges, against what an M8 thread 20 mm long
// costs: the numbers behind DefaultHelixSplit. Run: bz_tests -ts=measure -tc="*thread helix*" -s
TEST_CASE("measure: thread helix split" * doctest::test_suite("measure")) {
    using kernel::occt::HelixSplit;
    const HelixSplit was = kernel::occt::DefaultHelixSplit();
    for (HelixSplit split : {HelixSplit::HalfTurns, HelixSplit::Turns, HelixSplit::Single}) {
        kernel::occt::SetHelixSplitForMeasurement(split);
        doc::Document d;
        auto& sk = AddSketch(d);
        AddCircle(d, sk, {0, 0}, 4);
        AddExtrude(d, sk.id, "20");
        auto& thread = AddThread(d, 4, "1.25");
        auto& top = AddSketch(d);
        top.placement.frame.origin = {0, 0, 20};
        AddCircle(d, top, {3.5, 0}, 1.5);
        auto& cut = AddExtrude(d, top.id, "6", doc::BooleanOp::Cut);
        cut.reversed = true;
        kernel::Model model;
        const auto t = std::chrono::steady_clock::now();
        model.Rebuild(d);
        const double whole = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
        MESSAGE("thread split " << static_cast<int>(split) << ": thread " << thread.status.state << " "
                                << model.GetImpl().costMs.at(thread.id) << " ms, cut after it " << model.GetImpl().costMs.at(cut.id)
                                << " ms (" << cut.status.state << "), whole " << whole << " ms, volume " << model.Volume());
    }
    kernel::occt::SetHelixSplitForMeasurement(was);
}
