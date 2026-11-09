// Import of STEP and STL (kernel/Import.h) and «Импортированное тело» in a part.
// The STEP files are written here by OCCT itself; nothing binary is kept in
// the repository.

#include "../Fixtures.h"
#include "core/PathText.h"
#include "document/BzcFile.h"
#include "document/EdgeFeature.h"
#include "document/ImportFeature.h"
#include "kernel/Import.h"
#include "kernel/Model.h"
#include "kernel/Projection.h"
#include "kernel/Topology.h"
#include "kernel/occt/Import.h"
#include "kernel/occt/ModelImpl.h"

#include <BRepAlgoAPI_Cut.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <Interface_Static.hxx>
#include <STEPControl_Writer.hxx>
#include <TopoDS_Compound.hxx>
#include <gp_Pln.hxx>

#include <doctest.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <numbers>
#include <string>
#include <thread>

using namespace bz;
using namespace bz::test;
using doc::FeatureState;
namespace fs = std::filesystem;

namespace {


fs::path TempFile(const std::string& name) {
    const fs::path dir = fs::temp_directory_path() / "bz-import-tests";
    fs::create_directories(dir);
    return dir / PathFromUtf8(name);
}

/// Writes `shape` as STEP AP214 with lengths in `unit` ("MM", "INCH", "M").
void WriteStep(const TopoDS_Shape& shape, const fs::path& path, const char* unit = "MM") {
    STEPControl_Writer writer;
    Interface_Static::SetCVal("write.step.schema", "AP214IS");
    Interface_Static::SetCVal("write.step.unit", unit);
    REQUIRE(writer.Transfer(shape, STEPControl_AsIs) == IFSelect_RetDone);
    const std::string utf8 = ToUtf8(path);
    REQUIRE(writer.Write(utf8.c_str()) == IFSelect_RetDone);
    Interface_Static::SetCVal("write.step.unit", "MM");
}

void WriteText(const fs::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

std::string ReadText(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

/// The imported feature added to a new part, as «Импорт» will do it.
doc::ImportFeature& AddImported(doc::Document& d, doc::ImportFeature f) {
    auto owned = std::make_unique<doc::ImportFeature>(std::move(f));
    owned->id = d.Ids().Next();
    owned->name = d.UniqueFeatureName("Импортированное тело");
    auto* raw = owned.get();
    d.Insert(std::move(owned), d.FeatureCount());
    return *raw;
}

doc::ImportFeature Imported(const fs::path& path, const kernel::ImportOptions& options = {}) {
    Expected<kernel::ImportResult> r = kernel::ImportFile(path, options);
    if (!r) FAIL(r.GetError().message);
    return std::move(r->feature);
}

/// Binary STL from raw triangles.
void WriteBinaryStl(const fs::path& path, const std::vector<std::array<float, 9>>& tris, std::size_t truncateTo = 0) {
    std::string bytes(80, ' ');
    const auto n = static_cast<std::uint32_t>(tris.size());
    bytes.append(reinterpret_cast<const char*>(&n), 4);
    for (const auto& t : tris) {
        const float normal[3] = {0, 0, 0};
        bytes.append(reinterpret_cast<const char*>(normal), 12);
        bytes.append(reinterpret_cast<const char*>(t.data()), 36);
        bytes.append(2, '\0');
    }
    if (truncateTo) bytes.resize(truncateTo);
    WriteText(path, bytes);
}

/// A closed UV sphere: 2·rings·segments triangles, outward.
std::vector<std::array<float, 9>> Sphere(double r, int rings, int segments) {
    const auto at = [&](int i, int j) {
        const double theta = std::numbers::pi * i / rings, phi = 2 * std::numbers::pi * j / segments;
        return std::array<float, 3>{float(r * std::sin(theta) * std::cos(phi)), float(r * std::sin(theta) * std::sin(phi)),
                                    float(r * std::cos(theta))};
    };
    std::vector<std::array<float, 9>> tris;
    tris.reserve(static_cast<std::size_t>(2 * rings * segments));
    for (int i = 0; i < rings; ++i)
        for (int j = 0; j < segments; ++j) {
            const auto a = at(i, j), b = at(i + 1, j), c = at(i + 1, j + 1), e = at(i, j + 1);
            tris.push_back({a[0], a[1], a[2], b[0], b[1], b[2], c[0], c[1], c[2]});
            tris.push_back({a[0], a[1], a[2], c[0], c[1], c[2], e[0], e[1], e[2]});
        }
    return tris;
}

doc::EdgeReference CubeTopFrontEdge() {
    // Cube 0…20: the edge along X at y = 0, z = 20.
    doc::EdgeReference e;
    e.curve = doc::CurveKind::Line;
    e.direction = {1, 0, 0};
    e.middle = {10, 0, 20};
    e.faces = {{{doc::SurfaceKind::Plane, {0, -1, 0}}, {doc::SurfaceKind::Plane, {0, 0, 1}}}};
    return e;
}

} // namespace

// --- Document ------------------------------------------------------------------

TEST_CASE("base64 round-trips every byte and rejects garbage") {
    std::string all;
    for (int i = 0; i < 256; ++i) all += static_cast<char>(i);
    for (std::size_t n : {0u, 1u, 2u, 3u, 4u, 255u, 256u}) {
        const std::string part = all.substr(0, n);
        std::string back;
        REQUIRE(doc::DecodeBase64(doc::EncodeBase64(part), back));
        CHECK(back == part);
    }
    std::string out = "kept";
    CHECK_FALSE(doc::DecodeBase64("abc", out));     // length not a multiple of 4
    CHECK_FALSE(doc::DecodeBase64("ab!d", out));    // not in the alphabet
    CHECK_FALSE(doc::DecodeBase64("a=bc", out));    // padding in the middle
    CHECK(out == "kept");
}

// --- STEP ----------------------------------------------------------------------

TEST_CASE("STEP cube imports as one solid of the right volume and survives save and load") {
    const fs::path path = TempFile("куб.step");   // Cyrillic name: Windows paths are wide
    WriteStep(BRepPrimAPI_MakeBox(20, 20, 20).Shape(), path);

    Expected<kernel::ImportProbe> probe = kernel::ProbeImport(path);
    REQUIRE(probe);
    CHECK(probe->format == kernel::ImportFormat::Step);
    CHECK_FALSE(kernel::IsLargeImport(*probe));

    doc::Document d;
    doc::ImportFeature& f = AddImported(d, Imported(path));
    CHECK(f.kind == doc::ImportKind::Brep);
    CHECK(f.bodyCount == 1);
    CHECK(f.sourceFormat == "STEP");
    CHECK(f.source == ToUtf8(path));

    kernel::Model m;
    m.Rebuild(d);
    CHECK(f.status.state == FeatureState::Ok);
    REQUIRE(m.Bodies().size() == 1);
    CHECK(m.Volume() == doctest::Approx(8000.0).epsilon(1e-6));

    // Geometry is inside the part file: it opens with the STEP gone.
    const std::string json = doc::DocumentToJson(d);
    fs::remove(path);
    Expected<doc::Document> loaded = doc::DocumentFromJson(json);
    REQUIRE(loaded);
    kernel::Model again;
    again.Rebuild(*loaded);
    REQUIRE(again.Bodies().size() == 1);
    CHECK(again.Volume() == doctest::Approx(8000.0).epsilon(1e-6));
    const auto* back = dynamic_cast<const doc::ImportFeature*>(loaded->Features().front().get());
    REQUIRE(back);
    CHECK(back->source == f.source);
}

TEST_CASE("operations work on an imported STEP body: cut, fillet, chamfer, topology") {
    const fs::path path = TempFile("cube-ops.step");
    WriteStep(BRepPrimAPI_MakeBox(20, 20, 20).Shape(), path);

    SUBCASE("a sketch cut through it") {
        doc::Document d;
        AddImported(d, Imported(path));
        doc::SketchFeature& sk = AddSketch(d);
        AddCircle(d, sk, {10, 10}, 4);
        doc::ExtrudeFeature& cut = AddExtrude(d, sk.id, "20", doc::BooleanOp::Cut);
        kernel::Model m;
        m.Rebuild(d);
        CHECK(cut.status.state == FeatureState::Ok);
        REQUIRE(m.Bodies().size() == 1);
        CHECK(m.Volume() == doctest::Approx(8000.0 - std::numbers::pi * 16 * 20).epsilon(1e-4));
    }
    SUBCASE("a boss joined to it stays one body") {
        doc::Document d;
        AddImported(d, Imported(path));
        doc::SketchFeature& sk = AddSketch(d);
        AddRect(d, sk, {5, 5}, {15, 15});
        doc::ExtrudeFeature& boss = AddExtrude(d, sk.id, "30", doc::BooleanOp::Join);
        kernel::Model m;
        m.Rebuild(d);
        CHECK(boss.status.state == FeatureState::Ok);
        CHECK(m.Bodies().size() == 1);
        CHECK(m.Volume() == doctest::Approx(8000.0 + 100 * 10).epsilon(1e-4));
    }
    SUBCASE("fillet and chamfer on its edges") {
        for (bool fillet : {true, false}) {
            doc::Document d;
            AddImported(d, Imported(path));
            std::unique_ptr<doc::EdgeFeature> op;
            if (fillet) op = std::make_unique<doc::FilletFeature>();
            else op = std::make_unique<doc::ChamferFeature>();
            op->id = d.Ids().Next();
            op->name = "Операция";
            op->edges = {CubeTopFrontEdge()};
            std::vector<Parameter*> params;
            op->CollectParameters(params);
            for (Parameter* p : params) p->expression = "2";
            doc::EdgeFeature& raw = *op;
            d.Insert(std::move(op), d.FeatureCount());
            kernel::Model m;
            m.Rebuild(d);
            CHECK(raw.status.state == FeatureState::Ok);
            const double removed = fillet ? (4 - std::numbers::pi) * 20 : 2.0 * 20;   // r²(1−π/4)·L ; ½·2·2·L
            CHECK(m.Volume() == doctest::Approx(8000.0 - removed).epsilon(1e-4));
        }
    }
    SUBCASE("faces and edges are named like any body's") {
        doc::Document d;
        AddImported(d, Imported(path));
        kernel::Model m;
        m.Rebuild(d);
        const kernel::Topology t = kernel::DescribeTopology(m);
        CHECK(t.faces.size() == 6);
        CHECK(t.edges.size() == 12);
    }
}

TEST_CASE("STEP cylinder with a hole keeps its exact volume") {
    const fs::path path = TempFile("cylinder-hole.stp");
    const TopoDS_Shape outer = BRepPrimAPI_MakeCylinder(10, 30).Shape();
    const TopoDS_Shape inner = BRepPrimAPI_MakeCylinder(4, 30).Shape();
    WriteStep(BRepAlgoAPI_Cut(outer, inner).Shape(), path);
    doc::Document d;
    AddImported(d, Imported(path));
    kernel::Model m;
    m.Rebuild(d);
    REQUIRE(m.Bodies().size() == 1);
    CHECK(m.Volume() == doctest::Approx(std::numbers::pi * (100 - 16) * 30).epsilon(1e-6));
    // Its cylinders are recognised (holes for later stages, AutoDrop...).
    int holes = 0;
    for (const kernel::FaceInfo& face : kernel::DescribeTopology(m).faces) holes += face.hole ? 1 : 0;
    CHECK(holes == 1);
}

TEST_CASE("several bodies in a STEP file become several bodies of one part") {
    const fs::path path = TempFile("three-bodies.step");
    TopoDS_Compound c;
    BRep_Builder b;
    b.MakeCompound(c);
    b.Add(c, BRepPrimAPI_MakeBox(gp_Pnt(0, 0, 0), 10, 10, 10).Shape());
    b.Add(c, BRepPrimAPI_MakeBox(gp_Pnt(20, 0, 0), 10, 10, 10).Shape());
    b.Add(c, BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(50, 0, 0), gp_Dir(0, 0, 1)), 5, 10).Shape());
    WriteStep(c, path);
    doc::Document d;
    doc::ImportFeature& f = AddImported(d, Imported(path));
    CHECK(f.bodyCount == 3);
    kernel::Model m;
    m.Rebuild(d);
    REQUIRE(m.Bodies().size() == 3);
    CHECK(m.Bodies()[0].name == "Тело1");
    CHECK(m.Bodies()[2].name == "Тело3");
    CHECK(m.Volume() == doctest::Approx(2000.0 + std::numbers::pi * 25 * 10).epsilon(1e-6));
    // A new body after them is numbered on.
    doc::SketchFeature& sk = AddSketch(d);
    AddRect(d, sk, {100, 100}, {110, 110});
    AddExtrude(d, sk.id, "5");
    m.Rebuild(d);
    REQUIRE(m.Bodies().size() == 4);
    CHECK(m.Bodies()[3].name == "Тело4");
}

TEST_CASE("STEP in inches comes in millimetres") {
    const fs::path path = TempFile("inch.step");
    // A file from a CAD working in inches: a 1 × 1 × 2 inch block (OCCT
    // writes the numbers as they are and names the unit INCH).
    WriteStep(BRepPrimAPI_MakeBox(1, 1, 2).Shape(), path, "INCH");
    const std::string text = ReadText(path);
    CHECK(text.find("INCH") != std::string::npos);
    doc::Document d;
    doc::ImportFeature& f = AddImported(d, Imported(path));
    CHECK(f.sourceUnit == "INCH");
    kernel::Model m;
    m.Rebuild(d);
    CHECK(m.Volume() == doctest::Approx(25.4 * 25.4 * 50.8).epsilon(1e-6));
    const auto bounds = m.BoundingBox();
    REQUIRE(bounds);
    CHECK(bounds->max.z - bounds->min.z == doctest::Approx(50.8).epsilon(1e-4));
}

TEST_CASE("STEP with only an open surface imports it as a surface") {
    const fs::path path = TempFile("surface.step");
    WriteStep(BRepBuilderAPI_MakeFace(gp_Pln(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), 0, 10, 0, 10).Shape(), path);
    Expected<kernel::ImportResult> r = kernel::ImportFile(path);
    REQUIRE(r);
    CHECK_FALSE(r->warnings.empty());
    doc::Document d;
    doc::ImportFeature& f = AddImported(d, std::move(r->feature));
    kernel::Model m;
    m.Rebuild(d);
    CHECK(f.status.state == FeatureState::Warning);
    CHECK(m.Bodies().size() == 1);
    CHECK(m.Volume() == doctest::Approx(0.0));
}

TEST_CASE("broken files give a clear message, not a crash") {
    const fs::path good = TempFile("good.step");
    WriteStep(BRepPrimAPI_MakeBox(20, 20, 20).Shape(), good);
    const std::string text = ReadText(good);

    const auto fails = [](const fs::path& path) {
        Expected<kernel::ImportResult> r = kernel::ImportFile(path);
        CHECK_FALSE(r);
        if (!r) {
            CHECK_FALSE(r.GetError().message.empty());
            return r.GetError().message;
        }
        return std::string{};
    };
    SUBCASE("empty file") {
        const fs::path p = TempFile("empty.step");
        WriteText(p, "");
        fails(p);
    }
    SUBCASE("not a STEP at all") {
        const fs::path p = TempFile("picture.step");
        WriteText(p, std::string("\x89PNG\r\n\x1a\n", 8) + std::string(500, 'x'));
        CHECK(fails(p).find("STEP") != std::string::npos);
    }
    SUBCASE("cut off in the middle") {
        const fs::path p = TempFile("truncated.step");
        WriteText(p, text.substr(0, text.size() / 2));
        // OCCT may still read what is there; it must either import less or fail cleanly.
        Expected<kernel::ImportResult> r = kernel::ImportFile(p);
        if (r) CHECK(r->feature.data);
        else CHECK_FALSE(r.GetError().message.empty());
    }
    SUBCASE("header only, no geometry") {
        const fs::path p = TempFile("header-only.step");
        const std::size_t data = text.find("DATA;");
        REQUIRE(data != std::string::npos);
        WriteText(p, text.substr(0, data) + "DATA;\nENDSEC;\nEND-ISO-10303-21;\n");
        fails(p);
    }
    SUBCASE("garbage in the entities") {
        const fs::path p = TempFile("garbage.step");
        std::string bad = text;
        for (std::size_t i = bad.find("DATA;") + 10; i < bad.size(); i += 37) bad[i] = '#';
        WriteText(p, bad);
        Expected<kernel::ImportResult> r = kernel::ImportFile(p);
        if (!r) CHECK_FALSE(r.GetError().message.empty());
    }
    SUBCASE("missing file") {
        fails(TempFile("нет такого.step"));
    }
    SUBCASE("unknown format") {
        const fs::path p = TempFile("notes.txt");
        WriteText(p, "hello");
        CHECK(fails(p).find("STEP") != std::string::npos);
    }
}

TEST_CASE("a damaged data block in the part file keeps the part openable and the bytes unchanged") {
    const fs::path path = TempFile("cube-damaged.step");
    WriteStep(BRepPrimAPI_MakeBox(20, 20, 20).Shape(), path);
    doc::Document d;
    AddImported(d, Imported(path));
    doc::SketchFeature& sk = AddSketch(d);
    AddRect(d, sk, {100, 100}, {110, 110});
    AddExtrude(d, sk.id, "5");

    nlohmann::json json = nlohmann::json::parse(doc::DocumentToJson(d));
    std::string damagedData;
    for (auto& f : json["features"])
        if (f.value("type", "") == "importedBody") {
            std::string& data = f["data"].get_ref<std::string&>();
            data[data.size() / 2] = data[data.size() / 2] == 'A' ? 'B' : 'A';
            damagedData = data;
        }
    REQUIRE_FALSE(damagedData.empty());
    doc::LoadReport report;
    Expected<doc::Document> loaded = doc::DocumentFromJson(json.dump(), &report);
    REQUIRE(loaded);
    CHECK_FALSE(report.warnings.empty());
    kernel::Model m;
    m.Rebuild(*loaded);
    // Kept the way the container keeps any operation it cannot read: as it is
    // in the file, not built, with the reason.
    const doc::Feature& f = *loaded->Features().front();
    CHECK(f.Type() == doc::ImportFeature::kType);
    CHECK(dynamic_cast<const doc::ImportFeature*>(&f) == nullptr);
    CHECK(f.status.state == FeatureState::Error);
    CHECK(f.status.message.find("повреждены") != std::string::npos);
    CHECK(m.Bodies().size() == 1);   // the rest of the part is still there
    // Saved again: the damaged bytes go back unchanged (they may still be rescued).
    const nlohmann::json again = nlohmann::json::parse(doc::DocumentToJson(*loaded));
    bool same = false;
    for (const auto& g : again["features"])
        if (g.value("type", "") == "importedBody") same = g.value("data", "") == damagedData;
    CHECK(same);
}

TEST_CASE("an import can be cancelled") {
    const fs::path path = TempFile("cancel.step");
    WriteStep(BRepPrimAPI_MakeBox(20, 20, 20).Shape(), path);
    kernel::ImportProgress progress;
    progress.cancel = true;
    Expected<kernel::ImportResult> r = kernel::ImportFile(path, {}, &progress);
    REQUIRE_FALSE(r);
    CHECK(r.GetError().message == kernel::kImportCancelled);

    const fs::path stl = TempFile("cancel.stl");
    WriteBinaryStl(stl, Sphere(10, 100, 100));
    kernel::ImportProgress p2;
    p2.cancel = true;
    Expected<kernel::ImportResult> r2 = kernel::ImportFile(stl, {}, &p2);
    REQUIRE_FALSE(r2);
    CHECK(r2.GetError().message == kernel::kImportCancelled);
}

// --- STL -----------------------------------------------------------------------

TEST_CASE("binary STL exported by BreezeCAD comes back as a mesh body") {
    // A cube made the usual way, exported, imported into a new part.
    doc::Document src;
    doc::SketchFeature& sk = AddSketch(src);
    AddRect(src, sk, {0, 0}, {20, 20});
    AddExtrude(src, sk.id, "20");
    kernel::Model srcModel;
    srcModel.Rebuild(src);
    const fs::path path = TempFile("cube.stl");
    REQUIRE(srcModel.ExportStl(path));

    Expected<kernel::ImportProbe> probe = kernel::ProbeImport(path);
    REQUIRE(probe);
    CHECK(probe->format == kernel::ImportFormat::Stl);
    CHECK(probe->binary);
    CHECK(probe->triangles == 12);

    doc::Document d;
    doc::ImportFeature& f = AddImported(d, Imported(path));
    CHECK(f.kind == doc::ImportKind::Mesh);
    CHECK(f.triangles == 12);
    kernel::Model m;
    m.Rebuild(d);
    CHECK(f.status.state == FeatureState::Ok);
    REQUIRE(m.Bodies().size() == 1);
    CHECK(m.GetImpl().bodies.front().mesh);
    CHECK(m.Volume() == doctest::Approx(8000.0).epsilon(1e-5));
    const auto props = m.Properties();
    REQUIRE(props);
    CHECK(props->area == doctest::Approx(2400.0).epsilon(1e-5));
    CHECK(props->centre.x == doctest::Approx(10.0).epsilon(1e-5));
    CHECK(props->bounds.max.z == doctest::Approx(20.0).epsilon(1e-5));
    CHECK(m.BodyNearest({10, 10, 25}) == f.id);
    CHECK(kernel::DescribeTopology(m).faces.empty());   // nothing to name on triangles

    // Saved in the part and read back.
    Expected<doc::Document> loaded = doc::DocumentFromJson(doc::DocumentToJson(d));
    REQUIRE(loaded);
    kernel::Model again;
    again.Rebuild(*loaded);
    CHECK(again.Volume() == doctest::Approx(8000.0).epsilon(1e-5));

    // Exported again: STL yes, STEP no (no surfaces).
    const fs::path out = TempFile("cube-again.stl");
    REQUIRE(m.ExportStl(out));
    CHECK(fs::file_size(out) == 84 + 12 * 50);
    CHECK_FALSE(m.ExportStep(TempFile("cube-again.step")));
}

TEST_CASE("mesh body is a reference: solids next to it are not merged into it") {
    const fs::path path = TempFile("ref.stl");
    WriteBinaryStl(path, Sphere(10, 20, 20));
    doc::Document d;
    AddImported(d, Imported(path));
    // A join right through the mesh: it makes its own body, the mesh stays.
    doc::SketchFeature& sk = AddSketch(d);
    AddRect(d, sk, {-5, -5}, {5, 5});
    doc::ExtrudeFeature& boss = AddExtrude(d, sk.id, "30", doc::BooleanOp::Join);
    doc::SketchFeature& sk2 = AddSketch(d);
    AddCircle(d, sk2, {0, 0}, 2);
    doc::ExtrudeFeature& cut = AddExtrude(d, sk2.id, "30", doc::BooleanOp::Cut);
    kernel::Model m;
    m.Rebuild(d);
    CHECK(boss.status.state == FeatureState::Ok);
    CHECK(cut.status.state == FeatureState::Ok);
    REQUIRE(m.Bodies().size() == 2);
    CHECK(m.GetImpl().bodies[0].mesh);
    CHECK_FALSE(m.GetImpl().bodies[1].mesh);
    CHECK(kernel::occt::MeasureMesh(m.GetImpl().bodies[0].shape).triangles > 0);

    // A cut with only the mesh in the part is an error, not a crash.
    doc::Document only;
    AddImported(only, Imported(path));
    doc::SketchFeature& s3 = AddSketch(only);
    AddCircle(only, s3, {0, 0}, 2);
    doc::ExtrudeFeature& lonely = AddExtrude(only, s3.id, "30", doc::BooleanOp::Cut);
    kernel::Model m2;
    m2.Rebuild(only);
    CHECK(lonely.status.state == FeatureState::Error);
    CHECK(m2.Bodies().size() == 1);
}

TEST_CASE("mesh body keeps its triangles through meshing and a background rebuild") {
    const fs::path path = TempFile("keep.stl");
    WriteBinaryStl(path, Sphere(10, 20, 20));
    doc::Document d;
    const std::size_t triangles = AddImported(d, Imported(path)).triangles;   // 780: the poles' slivers go
    kernel::Model m;
    m.Rebuild(d);
    const TopoDS_Shape mesh = m.GetImpl().bodies.front().shape;
    // What the viewport and the previews do to every body.
    BRepMesh_IncrementalMesh(mesh, 0.01, false, 0.5, false);
    CHECK(kernel::occt::MeasureMesh(mesh).triangles == triangles);

    // The background rebuild works on a fork from after the import.
    doc::SketchFeature& sk = AddSketch(d);
    AddRect(d, sk, {30, 30}, {40, 40});
    AddExtrude(d, sk.id, "5");
    kernel::Model fork = m.Fork(1);
    doc::Document copy = d.Clone();
    fork.Rebuild(copy);
    m.Adopt(std::move(fork), 1);
    REQUIRE(m.GetImpl().bodiesAfter.back().size() == 2);
    CHECK(kernel::occt::MeasureMesh(m.GetImpl().bodiesAfter.back().front().shape).triangles == triangles);
}

TEST_CASE("text STL: any layout, locale-independent numbers, broken facets reported") {
    const std::string tetra =
        "solid t\n"
        "  facet normal 0 0 -1\n    outer loop\n      vertex 0 0 0\n      vertex 0 10 0\n      vertex 10 0 0\n    endloop\n  endfacet\n"
        "  facet normal 0 -1 0\n    outer loop\n      vertex 0 0 0\n      vertex 10 0 0\n      vertex 0 0 1.0e1\n    endloop\n  endfacet\n"
        "  facet normal -1 0 0\n    outer loop\n      vertex 0 0 0\n      vertex 0 0 10\n      vertex 0 10 0\n    endloop\n  endfacet\n"
        "\tfacet normal 1 1 1\n outer loop\n vertex 10 0 0\n vertex +0 10 0\n vertex 0 0 10.000\n endloop\n endfacet\n"
        "endsolid t\n";
    const fs::path path = TempFile("tetra.stl");
    WriteText(path, tetra);
    Expected<kernel::ImportProbe> probe = kernel::ProbeImport(path);
    REQUIRE(probe);
    CHECK_FALSE(probe->binary);
    doc::Document d;
    doc::ImportFeature& f = AddImported(d, Imported(path));
    CHECK(f.triangles == 4);
    kernel::Model m;
    m.Rebuild(d);
    CHECK(m.Volume() == doctest::Approx(1000.0 / 6.0).epsilon(1e-6));

    std::string broken = tetra;
    broken.replace(broken.find("vertex 0 10 0"), 13, "vertex 0 abc 0");
    const fs::path bad = TempFile("tetra-broken.stl");
    WriteText(bad, broken);
    Expected<kernel::ImportResult> r = kernel::ImportFile(bad);
    REQUIRE_FALSE(r);
    CHECK(r.GetError().message.find("строка 5") != std::string::npos);
}

TEST_CASE("binary STL: truncated, degenerate and garbage files") {
    SUBCASE("truncated keeps the whole triangles with a warning") {
        const fs::path p = TempFile("truncated.stl");
        WriteBinaryStl(p, Sphere(10, 10, 10), 84 + 50 * 150 + 20);
        Expected<kernel::ImportResult> r = kernel::ImportFile(p);
        REQUIRE(r);
        CHECK(r->feature.triangles <= 150);
        CHECK_FALSE(r->warnings.empty());
    }
    SUBCASE("degenerate triangles are dropped") {
        const fs::path p = TempFile("degenerate.stl");
        auto tris = Sphere(10, 10, 10);   // the poles already give some degenerate ones
        tris.push_back({1, 1, 1, 1, 1, 1, 2, 2, 2});
        WriteBinaryStl(p, tris);
        Expected<kernel::ImportResult> r = kernel::ImportFile(p);
        REQUIRE(r);
        CHECK(r->feature.triangles < tris.size());
        CHECK_FALSE(r->warnings.empty());
    }
    SUBCASE("NaN coordinates") {
        const fs::path p = TempFile("nan.stl");
        WriteBinaryStl(p, {{0, 0, 0, std::nanf(""), 0, 0, 0, 1, 0}});
        Expected<kernel::ImportResult> r = kernel::ImportFile(p);
        REQUIRE_FALSE(r);
        CHECK(r.GetError().message.find("нечисловыми") != std::string::npos);
    }
    SUBCASE("too short") {
        const fs::path p = TempFile("short.stl");
        WriteText(p, "abc");
        CHECK_FALSE(kernel::ImportFile(p));
    }
}

TEST_CASE("dense STL is simplified to the limit, keeping its shape") {
    const fs::path path = TempFile("dense.stl");
    WriteBinaryStl(path, Sphere(50, 300, 300));   // 180 000 triangles
    kernel::ImportOptions options;
    options.maxTriangles = 20'000;
    Expected<kernel::ImportResult> r = kernel::ImportFile(path, options);
    REQUIRE(r);
    CHECK(r->feature.triangles <= 20'000);
    CHECK(r->feature.triangles > 5'000);
    CHECK(r->feature.originalTriangles > 170'000);
    doc::Document d;
    AddImported(d, std::move(r->feature));
    kernel::Model m;
    m.Rebuild(d);
    CHECK(m.Volume() == doctest::Approx(4.0 / 3.0 * std::numbers::pi * 50 * 50 * 50).epsilon(0.02));
}

TEST_CASE("large files: probe warns, import takes a measured time") {
    // ~2 million triangles, ~100 MB: the size of a detailed scan.
    const fs::path stl = TempFile("large.stl");
    WriteBinaryStl(stl, Sphere(80, 1000, 1000));
    Expected<kernel::ImportProbe> probe = kernel::ProbeImport(stl);
    REQUIRE(probe);
    CHECK(probe->triangles == 2'000'000);
    CHECK(probe->triangles > kernel::kMeshTriangleLimitDesktop);

    kernel::ImportOptions options;
    options.maxTriangles = kernel::kMeshTriangleLimitPhone;
    kernel::ImportProgress progress;
    double lastSeen = 0;
    const auto start = std::chrono::steady_clock::now();
    Expected<kernel::ImportResult> r = Error{};
    std::atomic<bool> done{false};
    std::thread worker([&] {
        r = kernel::ImportFile(stl, options, &progress);
        done = true;
    });
    bool monotonic = true;
    while (!done) {
        const double now = progress.fraction;
        monotonic = monotonic && now + 1e-9 >= lastSeen;
        lastSeen = now;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    worker.join();
    const double stlSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    REQUIRE(r);
    CHECK(monotonic);
    CHECK(r->feature.triangles <= kernel::kMeshTriangleLimitPhone);
    MESSAGE("STL 2 000 000 → " << r->feature.triangles << " triangles: " << stlSeconds << " s, block "
                               << r->feature.data->size() / 1024 << " KiB");

    // A STEP with 400 bodies, each a plate with a hole.
    const fs::path step = TempFile("large.step");
    TopoDS_Compound c;
    BRep_Builder b;
    b.MakeCompound(c);
    const TopoDS_Shape plate =
        BRepAlgoAPI_Cut(BRepPrimAPI_MakeBox(10, 10, 2).Shape(),
                        BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(5, 5, 0), gp_Dir(0, 0, 1)), 2, 2).Shape())
            .Shape();
    for (int i = 0; i < 20; ++i)
        for (int j = 0; j < 20; ++j) {
            gp_Trsf t;
            t.SetTranslation(gp_Vec(15.0 * i, 15.0 * j, 0));
            b.Add(c, BRepBuilderAPI_Transform(plate, t, true).Shape());
        }
    WriteStep(c, step);
    const auto stepStart = std::chrono::steady_clock::now();
    Expected<kernel::ImportResult> s = kernel::ImportFile(step);
    const double stepSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - stepStart).count();
    REQUIRE(s);
    CHECK(s->feature.bodyCount == 400);
    doc::Document d;
    AddImported(d, std::move(s->feature));
    kernel::Model m;
    const auto rebuildStart = std::chrono::steady_clock::now();
    m.Rebuild(d);
    const double rebuildSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - rebuildStart).count();
    CHECK(m.Bodies().size() == 400);
    MESSAGE("STEP " << fs::file_size(step) / 1024 << " KiB, 400 bodies: import " << stepSeconds << " s, open (decode) "
                    << rebuildSeconds << " s");
    fs::remove(stl);
}

TEST_CASE("a mesh is a sketch reference: its section is projected into the sketch") {
    const fs::path path = TempFile("section.stl");
    WriteBinaryStl(path, Sphere(10, 60, 120));
    doc::Document d;
    AddImported(d, Imported(path));
    kernel::Model m;
    m.Rebuild(d);
    Frame plane;   // XY through the centre
    plane.origin = {0, 0, 0};
    plane.normal = {0, 0, 1};
    plane.xDir = {1, 0, 0};
    const std::vector<kernel::ProjectedCurve> cut = m.ProjectSection(plane, 1);
    REQUIRE_FALSE(cut.empty());
    double length = 0, worst = 0;
    for (const auto& c : cut) {
        CHECK(c.kind == kernel::ProjectedCurve::Kind::Line);
        length += (c.end - c.start).Length();
        worst = std::max({worst, std::abs(c.start.Length() - 10), std::abs(c.end.Length() - 10)});
    }
    CHECK(length == doctest::Approx(2 * std::numbers::pi * 10).epsilon(0.01));
    CHECK(worst < 0.05);
    // Off the mesh: nothing to project.
    plane.origin = {0, 0, 20};
    CHECK(m.ProjectSection(plane, 1).empty());
}

TEST_CASE("an imported part saves to .bzc and opens elsewhere without the source file") {
    const fs::path step = TempFile("travel.step");
    WriteStep(BRepAlgoAPI_Cut(BRepPrimAPI_MakeCylinder(10, 30).Shape(), BRepPrimAPI_MakeCylinder(4, 30).Shape()).Shape(), step);
    const fs::path stl = TempFile("travel.stl");
    WriteBinaryStl(stl, Sphere(10, 30, 30));
    for (const fs::path& source : {step, stl}) {
        doc::Document d;
        AddImported(d, Imported(source));
        kernel::Model before;
        before.Rebuild(d);
        const fs::path part = TempFile("путешественник.bzc");
        REQUIRE(doc::SaveBzc(d, part, "tests"));
        fs::remove(source);   // the part goes to a phone; the STEP or STL stays behind
        Expected<doc::Document> loaded = doc::LoadBzc(part);
        REQUIRE(loaded);
        kernel::Model after;
        after.Rebuild(*loaded);
        CHECK(after.Bodies().size() == before.Bodies().size());
        CHECK(after.Volume() == doctest::Approx(before.Volume()).epsilon(1e-9));
        // A zip keeps the base64 block compact: the part is not much larger than its geometry.
        const auto* f = dynamic_cast<const doc::ImportFeature*>(loaded->Features().front().get());
        REQUIRE(f);
        CHECK(fs::file_size(part) < f->data->size() * 2 + 64 * 1024);
    }
}

// --- Golden files ----------------------------------------------------------------
// Parts with imported bodies saved by the first version of the format. They
// must always open with the same geometry: the data block (tags BZB1 and
// BZM1, BinTools version 3) is part of the file format. Never delete them;
// a new block version gets new files next to these.

namespace {
const fs::path kGolden = fs::path(BZ_SOURCE_DIR) / "tests" / "data" / "import" / "v1";   // like tests/data/drawings/v1

/// The parts the golden files were made from.
doc::Document GoldenStep() {
    const fs::path step = TempFile("golden.step");
    WriteStep(BRepAlgoAPI_Cut(BRepPrimAPI_MakeCylinder(10, 30).Shape(), BRepPrimAPI_MakeCylinder(4, 30).Shape()).Shape(), step,
              "INCH");   // the file says inches: 10 × 30 inch, 4 inch hole
    doc::Document d;
    AddImported(d, Imported(step));
    return d;
}
doc::Document GoldenStl() {
    const fs::path stl = TempFile("golden.stl");
    WriteBinaryStl(stl, Sphere(10, 12, 16));
    doc::Document d;
    AddImported(d, Imported(stl));
    return d;
}
} // namespace

TEST_CASE("write the import golden files" * doctest::skip()) {   // run with --no-skip -tc="write the import*"
    fs::create_directories(kGolden);
    REQUIRE(doc::SaveBzc(GoldenStep(), kGolden / "step-inch-cylinder.bzc", "golden"));
    REQUIRE(doc::SaveBzc(GoldenStl(), kGolden / "stl-sphere.bzc", "golden"));
}

TEST_CASE("golden files of imported bodies (format v1) open with the same geometry") {
    SUBCASE("STEP in inches: exact solid") {
        Expected<doc::Document> d = doc::LoadBzc(kGolden / "step-inch-cylinder.bzc");
        REQUIRE(d);
        const auto* f = dynamic_cast<const doc::ImportFeature*>(d->Features().front().get());
        REQUIRE(f);
        CHECK(f->kind == doc::ImportKind::Brep);
        CHECK(f->sourceUnit == "INCH");
        CHECK(f->data->substr(0, 4) == "BZB1");
        kernel::Model m;
        m.Rebuild(*d);
        CHECK(f->status.state == FeatureState::Ok);
        REQUIRE(m.Bodies().size() == 1);
        CHECK(m.Volume() == doctest::Approx(std::numbers::pi * (100 - 16) * 30 * 25.4 * 25.4 * 25.4).epsilon(1e-6));
    }
    SUBCASE("STL: mesh") {
        Expected<doc::Document> d = doc::LoadBzc(kGolden / "stl-sphere.bzc");
        REQUIRE(d);
        const auto* f = dynamic_cast<const doc::ImportFeature*>(d->Features().front().get());
        REQUIRE(f);
        CHECK(f->kind == doc::ImportKind::Mesh);
        CHECK(f->data->substr(0, 4) == "BZM1");
        kernel::Model m;
        m.Rebuild(*d);
        CHECK(f->status.state == FeatureState::Ok);
        REQUIRE(m.Bodies().size() == 1);
        // The same as the part the file was made from, to the last digit.
        kernel::Model fresh;
        doc::Document made = GoldenStl();
        fresh.Rebuild(made);
        CHECK(m.Volume() == doctest::Approx(fresh.Volume()).epsilon(1e-12));
        CHECK(f->triangles == 368);   // 2·12·16, minus 16 slivers at one pole
    }
}
