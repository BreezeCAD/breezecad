// SPDX-License-Identifier: MPL-2.0
// Reference parts (stabilization plan, stage 0): every .bzc in
// tests/data/corpus/<version>/ is rebuilt and compared with the
// <name>.expect.json written next to it — the state of each operation and,
// for each body, volume, area, bounding box, faces and edges. A change in
// the program or in OCCT that builds a saved part differently fails here,
// naming the first operation that differs.
//
// The expectations are written by hand only, with the reason in the commit:
//   BZ_CORPUS_WRITE=1 ./bz_tests -tc="corpus*"
// Private parts (not in this open repository): BZ_CORPUS_DIR=<folder>.
// An expectation records what the part does now, mistakes included: a fix
// that changes it updates the file on purpose.
#include "document/BzcFile.h"
#include "document/Document.h"
#include "kernel/Model.h"
#include "kernel/occt/ModelImpl.h"

#include <BRepBndLib.hxx>
#include <BRepGProp.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopExp.hxx>

#include <doctest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace bz;
using nlohmann::json;

namespace {

const char* StateName(doc::FeatureState s) {
    switch (s) {
    case doc::FeatureState::NotComputed: return "not computed";
    case doc::FeatureState::Ok: return "ok";
    case doc::FeatureState::Warning: return "warning";
    case doc::FeatureState::Error: return "error";
    }
    return "?";
}

int Count(const TopoDS_Shape& shape, TopAbs_ShapeEnum kind) {
    TopTools_IndexedMapOfShape map;
    TopExp::MapShapes(shape, kind, map);
    return map.Extent();
}

/// What the part is now, in the form of an .expect.json.
json Describe(const doc::Document& d, const kernel::Model& model) {
    json out;
    out["kernel"] = kernel::GeometryKernelVersion();   // for the reader: not compared
    json features = json::array();
    for (const auto& f : d.Features())
        features.push_back({{"id", f->id.value}, {"name", f->name}, {"state", StateName(f->status.state)}});
    out["features"] = features;
    json bodies = json::array();
    for (const kernel::Body& b : model.GetImpl().bodies) {
        json body{{"name", b.name}, {"mesh", b.mesh}};
        if (!b.mesh) {
            GProp_GProps volume, surface;
            BRepGProp::VolumeProperties(b.shape, volume);
            BRepGProp::SurfaceProperties(b.shape, surface);
            Bnd_Box box;
            BRepBndLib::AddOptimal(b.shape, box, false, false);
            double x0 = 0, y0 = 0, z0 = 0, x1 = 0, y1 = 0, z1 = 0;
            if (!box.IsVoid()) box.Get(x0, y0, z0, x1, y1, z1);
            body["volume"] = volume.Mass();
            body["area"] = surface.Mass();
            body["box"] = {x0, y0, z0, x1, y1, z1};
            body["faces"] = Count(b.shape, TopAbs_FACE);
            body["edges"] = Count(b.shape, TopAbs_EDGE);
        }
        bodies.push_back(body);
    }
    out["bodies"] = bodies;
    return out;
}

bool Close(double a, double b, double relative, double absolute) {
    return std::abs(a - b) <= std::max(absolute, relative * std::max(std::abs(a), std::abs(b)));
}

/// The first difference, said as a person would, or "".
std::string Compare(const json& expected, const json& actual) {
    const json& ef = expected.at("features");
    const json& af = actual.at("features");
    if (ef.size() != af.size()) return "число операций: было " + std::to_string(ef.size()) + ", стало " + std::to_string(af.size());
    for (std::size_t i = 0; i < ef.size(); ++i)
        if (ef[i].at("state") != af[i].at("state"))
            return "операция «" + af[i].at("name").get<std::string>() + "» (№" + std::to_string(i + 1) + "): было " +
                   ef[i].at("state").get<std::string>() + ", стало " + af[i].at("state").get<std::string>();
    const json& eb = expected.at("bodies");
    const json& ab = actual.at("bodies");
    if (eb.size() != ab.size()) return "число тел: было " + std::to_string(eb.size()) + ", стало " + std::to_string(ab.size());
    for (std::size_t i = 0; i < eb.size(); ++i) {
        const json& e = eb[i];
        const json& a = ab[i];
        const std::string name = "тело «" + a.at("name").get<std::string>() + "»: ";
        if (e.value("mesh", false)) continue;
        for (const char* key : {"faces", "edges"})
            if (e.at(key) != a.at(key))
                return name + key + " было " + e.at(key).dump() + ", стало " + a.at(key).dump();
        for (const char* key : {"volume", "area"})
            if (!Close(e.at(key).get<double>(), a.at(key).get<double>(), 1e-6, 1e-9))
                return name + key + " было " + e.at(key).dump() + ", стало " + a.at(key).dump();
        for (std::size_t k = 0; k < 6; ++k)
            if (!Close(e.at("box")[k].get<double>(), a.at("box")[k].get<double>(), 0, 1e-6))
                return name + "габарит было " + e.at("box").dump() + ", стало " + a.at("box").dump();
    }
    return {};
}

/// The public reference parts (tests/data/corpus) and, when BZ_CORPUS_DIR is
/// set, the private ones: authors' and testers' real parts live in a closed
/// repository (BreezeCAD/bz-test-corpus), not in the open one; CI checks it
/// out next to the sources and points BZ_CORPUS_DIR at it.
std::vector<std::filesystem::path> CorpusFiles() {
    std::vector<std::filesystem::path> roots{std::filesystem::path(BZ_SOURCE_DIR) / "tests" / "data" / "corpus"};
    if (const char* extra = std::getenv("BZ_CORPUS_DIR"); extra && *extra) roots.emplace_back(extra);
    std::vector<std::filesystem::path> out;
    for (const std::filesystem::path& root : roots) {
        std::error_code ec;
        for (const auto& e : std::filesystem::recursive_directory_iterator(root, ec))
            if (e.path().extension() == ".bzc") out.push_back(e.path());
    }
    std::ranges::sort(out);
    return out;
}

} // namespace

TEST_CASE("corpus: every reference part builds as it did when its expectation was written") {
    const bool write = std::getenv("BZ_CORPUS_WRITE") != nullptr;
    const std::vector<std::filesystem::path> files = CorpusFiles();
    REQUIRE(!files.empty());
    for (const std::filesystem::path& file : files) {
        CAPTURE(file.string());
        Expected<doc::Document> d = doc::LoadBzc(file);
        REQUIRE(d);
        kernel::Model model;
        model.Rebuild(*d);
        const json actual = Describe(*d, model);
        std::filesystem::path expectPath = file;
        expectPath.replace_extension(".expect.json");
        if (write) {
            std::ofstream(expectPath, std::ios::binary) << actual.dump(1, '\t') << "\n";
            continue;
        }
        std::ifstream in(expectPath, std::ios::binary);
        REQUIRE_MESSAGE(in.good(), "нет эталона: запишите его BZ_CORPUS_WRITE=1");
        const json expected = json::parse(in);
        const std::string difference = Compare(expected, actual);
        CHECK_MESSAGE(difference.empty(), difference);
    }
}

