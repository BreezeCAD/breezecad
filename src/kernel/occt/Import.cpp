// STEP and STL import (kernel/Import.h) and decoding of imported bodies
// (occt/Import.h).
//
// Data block of doc::ImportFeature: 4-byte tag, uint64 size of the raw data
// (little-endian), then the raw data compressed with zlib (miniz):
//   "BZB1" — OCCT binary BRep (BinTools, format version 3, no triangles):
//            a compound whose children are the bodies;
//   "BZM1" — our own mesh: uint32 vertex count, uint32 triangle count,
//            float x y z per vertex, uint32 a b c per triangle (outward,
//            counter-clockwise). Independent of the OCCT version.
// BinTools version 3 is read by every OCCT the program builds with (7.6…8.0),
// so a part imported on one system opens on another.

#include "kernel/Import.h"
#include "kernel/occt/Import.h"

#include "core/FaultGuard.h"
#include "core/Log.h"
#include "core/PathText.h"

#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_MakeSolid.hxx>
#include <BRepBuilderAPI_Sewing.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <BinTools.hxx>
#include <GProp_GProps.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <Interface_Static.hxx>
#include <Message_ProgressIndicator.hxx>
#include <Message_ProgressScope.hxx>
#include <Poly_Triangulation.hxx>
#include <STEPControl_Reader.hxx>
#include <ShapeFix_Shape.hxx>
#include <ShapeFix_Solid.hxx>
#include <Standard_Failure.hxx>
#include <Standard_Version.hxx>
#include <TColStd_SequenceOfAsciiString.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Iterator.hxx>
#include <TopoDS_Shell.hxx>
#include <TopoDS_Solid.hxx>

#include <miniz.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <fstream>
#include <sstream>
#include <unordered_map>

namespace bz::kernel {
namespace {

// --- Data block -------------------------------------------------------------

constexpr std::string_view kBrepTag = "BZB1";
constexpr std::string_view kMeshTag = "BZM1";

std::string Pack(std::string_view tag, const std::string& raw) {
    mz_ulong bound = mz_compressBound(static_cast<mz_ulong>(raw.size()));
    std::string out(tag);
    std::uint64_t size = raw.size();
    char header[8];
    std::memcpy(header, &size, 8);   // every platform we build for is little-endian
    out.append(header, 8);
    const std::size_t start = out.size();
    out.resize(start + bound);
    if (mz_compress2(reinterpret_cast<unsigned char*>(out.data() + start), &bound,
                     reinterpret_cast<const unsigned char*>(raw.data()), static_cast<mz_ulong>(raw.size()), 6) != MZ_OK)
        return {};
    out.resize(start + bound);
    return out;
}

/// The raw data of a block with `tag`; error when the block is not one.
Expected<std::string> Unpack(const std::string& block, std::string_view& tag) {
    if (block.size() < 12) return Error{"Данные импортированного тела повреждены"};
    tag = std::string_view(block).substr(0, 4);
    if (tag != kBrepTag && tag != kMeshTag) return Error{"Данные импортированного тела в неизвестном формате (файл из более новой версии?)"};
    std::uint64_t size = 0;
    std::memcpy(&size, block.data() + 4, 8);
    if (size > (std::uint64_t{4} << 30)) return Error{"Данные импортированного тела повреждены"};
    std::string raw(static_cast<std::size_t>(size), '\0');
    mz_ulong length = static_cast<mz_ulong>(size);
    if (mz_uncompress(reinterpret_cast<unsigned char*>(raw.data()), &length,
                      reinterpret_cast<const unsigned char*>(block.data() + 12),
                      static_cast<mz_ulong>(block.size() - 12)) != MZ_OK ||
        length != size)
        return Error{"Данные импортированного тела повреждены"};
    return raw;
}

// --- Mesh -------------------------------------------------------------------

struct Mesh {
    std::vector<std::array<float, 3>>         vertices;
    std::vector<std::array<std::uint32_t, 3>> triangles;
};

std::string SerializeMesh(const Mesh& mesh) {
    std::string raw;
    raw.resize(8 + mesh.vertices.size() * 12 + mesh.triangles.size() * 12);
    const auto nv = static_cast<std::uint32_t>(mesh.vertices.size());
    const auto nt = static_cast<std::uint32_t>(mesh.triangles.size());
    std::memcpy(raw.data(), &nv, 4);
    std::memcpy(raw.data() + 4, &nt, 4);
    if (nv) std::memcpy(raw.data() + 8, mesh.vertices.data(), mesh.vertices.size() * 12);
    if (nt) std::memcpy(raw.data() + 8 + mesh.vertices.size() * 12, mesh.triangles.data(), mesh.triangles.size() * 12);
    return raw;
}

Expected<Mesh> DeserializeMesh(const std::string& raw) {
    if (raw.size() < 8) return Error{"Данные сетки повреждены"};
    std::uint32_t nv = 0, nt = 0;
    std::memcpy(&nv, raw.data(), 4);
    std::memcpy(&nt, raw.data() + 4, 4);
    if (raw.size() != 8 + std::uint64_t{nv} * 12 + std::uint64_t{nt} * 12) return Error{"Данные сетки повреждены"};
    Mesh mesh;
    mesh.vertices.resize(nv);
    mesh.triangles.resize(nt);
    if (nv) std::memcpy(mesh.vertices.data(), raw.data() + 8, std::size_t{nv} * 12);
    if (nt) std::memcpy(mesh.triangles.data(), raw.data() + 8 + std::size_t{nv} * 12, std::size_t{nt} * 12);
    for (const auto& t : mesh.triangles)
        for (std::uint32_t i : t)
            if (i >= nv) return Error{"Данные сетки повреждены"};
    return mesh;
}

/// One face with the triangles of `mesh` and no surface (single precision
/// keeps the memory down).
///
/// Vertices are shared where the surface is smooth and split along sharp
/// edges (crease over kCreaseDegrees): each side gets its own normal, so a
/// printed part keeps its sharp edges instead of being smoothed into a blob,
/// and OCCT draws those splits as the part's edges (it outlines triangle
/// edges with unshared nodes). Sharing nothing would outline every triangle.
TopoDS_Face MeshFace(const Mesh& mesh) {
    constexpr double kCreaseDegrees = 35.0;
    const double creaseCos = std::cos(kCreaseDegrees * 3.14159265358979323846 / 180.0);
    const std::size_t nt = mesh.triangles.size(), nv = mesh.vertices.size();

    // Face normals, area-weighted (unnormalised) and unit.
    std::vector<gp_XYZ> weighted(nt), unit(nt);
    for (std::size_t t = 0; t < nt; ++t) {
        const auto& idx = mesh.triangles[t];
        const auto P = [&](int k) {
            const auto& v = mesh.vertices[idx[static_cast<std::size_t>(k)]];
            return gp_XYZ(v[0], v[1], v[2]);
        };
        weighted[t] = (P(1) - P(0)).Crossed(P(2) - P(0));
        const double length = weighted[t].Modulus();
        unit[t] = length > 1e-30 ? weighted[t] / length : gp_XYZ(0, 0, 1);
    }
    // Triangles around each vertex (compressed rows).
    std::vector<std::uint32_t> start(nv + 1, 0), around(nt * 3);
    for (const auto& t : mesh.triangles)
        for (std::uint32_t v : t) ++start[v + 1];
    for (std::size_t v = 0; v < nv; ++v) start[v + 1] += start[v];
    {
        std::vector<std::uint32_t> fill(start.begin(), start.end() - 1);
        for (std::size_t t = 0; t < nt; ++t)
            for (std::uint32_t v : mesh.triangles[t]) around[fill[v]++] = static_cast<std::uint32_t>(t);
    }
    // Around each vertex, triangles facing alike share a node; the order is
    // the file's, so the same file always gives the same nodes.
    struct Node {
        std::uint32_t vertex;
        gp_XYZ        normal;   // sum of the weighted normals
        gp_XYZ        first;    // unit normal of the group's first triangle
    };
    std::vector<Node> nodes;
    nodes.reserve(nv + nv / 4);
    std::vector<std::array<std::uint32_t, 3>> corner(nt);
    for (std::uint32_t v = 0; v < nv; ++v) {
        const std::size_t base = nodes.size();
        for (std::uint32_t k = start[v]; k < start[v + 1]; ++k) {
            const std::uint32_t t = around[k];
            std::size_t g = base;
            for (; g < nodes.size(); ++g)
                if (nodes[g].first.Dot(unit[t]) >= creaseCos) break;
            if (g == nodes.size()) nodes.push_back({v, gp_XYZ(0, 0, 0), unit[t]});
            nodes[g].normal += weighted[t];
            const auto& idx = mesh.triangles[t];
            for (std::size_t c = 0; c < 3; ++c)
                if (idx[c] == v) corner[t][c] = static_cast<std::uint32_t>(g);
        }
    }

    Handle(Poly_Triangulation) tri = new Poly_Triangulation();
    tri->SetDoublePrecision(false);   // before the nodes are allocated
    tri->ResizeNodes(static_cast<int>(nodes.size()), false);
    tri->ResizeTriangles(static_cast<int>(nt), false);
    tri->AddNormals();
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        const auto& v = mesh.vertices[nodes[i].vertex];
        const int n = static_cast<int>(i) + 1;
        tri->SetNode(n, gp_Pnt(v[0], v[1], v[2]));
        const double length = nodes[i].normal.Modulus();
        const gp_XYZ d = length > 1e-30 ? nodes[i].normal / length : nodes[i].first;
        tri->SetNormal(n, gp_Vec3f(static_cast<float>(d.X()), static_cast<float>(d.Y()), static_cast<float>(d.Z())));
    }
    for (std::size_t t = 0; t < nt; ++t)
        tri->SetTriangle(static_cast<int>(t) + 1,
                         Poly_Triangle(static_cast<int>(corner[t][0]) + 1, static_cast<int>(corner[t][1]) + 1, static_cast<int>(corner[t][2]) + 1));
    TopoDS_Face face;
    BRep_Builder().MakeFace(face, tri);
    return face;
}

double MeshArea(const Mesh& mesh) {
    double area = 0.0;
    for (const auto& t : mesh.triangles) {
        const auto& a = mesh.vertices[t[0]];
        const auto& b = mesh.vertices[t[1]];
        const auto& c = mesh.vertices[t[2]];
        const Vec3 u{b[0] - a[0], b[1] - a[1], b[2] - a[2]}, v{c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        area += u.Cross(v).Length() / 2;
    }
    return area;
}

struct FloatKey {
    std::array<std::uint32_t, 3> bits;
    bool operator==(const FloatKey&) const = default;
};
struct FloatKeyHash {
    std::size_t operator()(const FloatKey& k) const {
        std::uint64_t h = 1469598103934665603ull;
        for (std::uint32_t b : k.bits) h = (h ^ b) * 1099511628211ull;
        return static_cast<std::size_t>(h);
    }
};

/// Collects triangles, sharing vertices with exactly the same coordinates
/// and dropping triangles with two equal corners.
class MeshBuilder {
public:
    explicit MeshBuilder(std::size_t expected) { myIndex.reserve(expected / 2 + 16); }
    void Add(const std::array<std::array<float, 3>, 3>& corners) {
        std::array<std::uint32_t, 3> t{};
        for (std::size_t k = 0; k < 3; ++k) t[k] = Vertex(corners[k]);
        if (t[0] == t[1] || t[1] == t[2] || t[0] == t[2]) {
            ++degenerate;
            return;
        }
        mesh.triangles.push_back(t);
    }
    Mesh          mesh;
    std::uint64_t degenerate = 0;

private:
    std::uint32_t Vertex(std::array<float, 3> v) {
        FloatKey key{};
        for (std::size_t k = 0; k < 3; ++k) {
            if (v[k] == 0.0f) v[k] = 0.0f;   // -0 and +0 are one point
            std::memcpy(&key.bits[k], &v[k], 4);
        }
        const auto [it, added] = myIndex.try_emplace(key, static_cast<std::uint32_t>(mesh.vertices.size()));
        if (added) mesh.vertices.push_back(v);
        return it->second;
    }
    std::unordered_map<FloatKey, std::uint32_t, FloatKeyHash> myIndex;
};

/// Simplifies by vertex clustering: vertices in one cell of a grid become
/// one (their average), triangles that collapse go. The cell grows until no
/// more than `limit` triangles are left. Deterministic, a few passes over
/// the mesh; fine detail is lost evenly over the whole part.
Mesh Simplify(const Mesh& in, std::uint64_t limit, ImportProgress* progress) {
    std::array<double, 3> lo{1e300, 1e300, 1e300};
    for (const auto& v : in.vertices)
        for (std::size_t k = 0; k < 3; ++k) lo[k] = std::min(lo[k], double(v[k]));
    // Vertices of a surface mesh go as area / cell²; about half as many vertices as triangles.
    double cell = std::sqrt(MeshArea(in) / std::max<double>(1.0, double(limit) / 2.0));
    if (!(cell > 0)) cell = 1e-3;
    Mesh out;
    for (int pass = 0; pass < 40; ++pass) {
        if (progress) progress->fraction = std::min(0.85, 0.45 + 0.01 * pass);
        if (progress && progress->cancel) return {};
        std::unordered_map<std::uint64_t, std::uint32_t> clusterOf;
        clusterOf.reserve(in.vertices.size() / 4 + 16);
        std::vector<std::uint32_t>         remap(in.vertices.size());
        std::vector<std::array<double, 4>> sums;
        for (std::size_t i = 0; i < in.vertices.size(); ++i) {
            const auto& v = in.vertices[i];
            std::uint64_t key = 0;
            for (std::size_t k = 0; k < 3; ++k)
                key = (key << 21) | (static_cast<std::uint64_t>(std::floor((v[k] - lo[k]) / cell)) & 0x1FFFFF);
            const auto [it, added] = clusterOf.try_emplace(key, static_cast<std::uint32_t>(sums.size()));
            if (added) sums.push_back({0, 0, 0, 0});
            auto& s = sums[it->second];
            s[0] += v[0], s[1] += v[1], s[2] += v[2], s[3] += 1;
            remap[i] = it->second;
        }
        out.vertices.resize(sums.size());
        for (std::size_t i = 0; i < sums.size(); ++i)
            for (std::size_t k = 0; k < 3; ++k) out.vertices[i][k] = static_cast<float>(sums[i][k] / sums[i][3]);
        out.triangles.clear();
        std::unordered_map<std::uint64_t, char> seen;   // a triangle once (both sides of a sliver collapse into one)
        for (const auto& t : in.triangles) {
            const std::array<std::uint32_t, 3> r{remap[t[0]], remap[t[1]], remap[t[2]]};
            if (r[0] == r[1] || r[1] == r[2] || r[0] == r[2]) continue;
            std::array<std::uint32_t, 3> sorted = r;
            std::sort(sorted.begin(), sorted.end());
            const std::uint64_t key = (std::uint64_t{sorted[0]} * 2654435761u) ^ (std::uint64_t{sorted[1]} << 21) ^
                                      (std::uint64_t{sorted[2]} << 42);
            if (!seen.try_emplace(key, 0).second) continue;
            out.triangles.push_back(r);
        }
        if (out.triangles.size() <= limit) break;
        cell *= 1.2;
    }
    // Vertices no triangle uses any more go.
    std::vector<std::uint32_t> used(out.vertices.size(), ~0u);
    Mesh packed;
    for (auto& t : out.triangles)
        for (auto& i : t) {
            if (used[i] == ~0u) {
                used[i] = static_cast<std::uint32_t>(packed.vertices.size());
                packed.vertices.push_back(out.vertices[i]);
            }
            i = used[i];
        }
    packed.triangles = std::move(out.triangles);
    return packed;
}

// --- STL reading ------------------------------------------------------------

Expected<std::string> ReadWhole(const std::filesystem::path& path, ImportProgress* progress, double share) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return Error{std::format("Не удалось открыть файл: {}", ToUtf8(path))};
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    in.seekg(0, std::ios::beg);
    if (size < 0) return Error{std::format("Не удалось прочитать файл: {}", ToUtf8(path))};
    std::string bytes(static_cast<std::size_t>(size), '\0');
    constexpr std::size_t kChunk = 8u << 20;
    for (std::size_t done = 0; done < bytes.size();) {
        const std::size_t n = std::min(kChunk, bytes.size() - done);
        if (!in.read(bytes.data() + done, static_cast<std::streamsize>(n)))
            return Error{std::format("Не удалось прочитать файл: {}", ToUtf8(path))};
        done += n;
        if (progress) {
            progress->fraction = share * double(done) / double(bytes.size());
            if (progress->cancel) return Error{kImportCancelled};
        }
    }
    return bytes;
}

bool LooksLikeTextStl(std::string_view bytes) {
    std::size_t i = 0;
    while (i < bytes.size() && std::isspace(static_cast<unsigned char>(bytes[i]))) ++i;
    if (bytes.substr(i, 5) != "solid") return false;
    // Some binary files start with "solid" in their header too: text ones have "facet" soon.
    return bytes.substr(0, std::min<std::size_t>(bytes.size(), 4096)).find("facet") != std::string_view::npos;
}

/// A decimal number, independent of the C locale (a Russian locale reads
/// "1,5" and stops at "1.5" otherwise). Advances `p`; false when there is none.
bool ParseNumber(const char*& p, const char* end, float& out) {
    while (p < end && (*p == ' ' || *p == '\t')) ++p;
    const char* start = p;
    bool negative = false;
    if (p < end && (*p == '+' || *p == '-')) negative = *p++ == '-';
    double value = 0.0;
    int digits = 0;
    while (p < end && *p >= '0' && *p <= '9') value = value * 10 + (*p++ - '0'), ++digits;
    if (p < end && *p == '.') {
        ++p;
        double scale = 0.1;
        while (p < end && *p >= '0' && *p <= '9') value += (*p++ - '0') * scale, scale *= 0.1, ++digits;
    }
    if (digits == 0) {
        p = start;
        return false;
    }
    if (p < end && (*p == 'e' || *p == 'E')) {
        const char* mark = p++;
        bool negExp = false;
        if (p < end && (*p == '+' || *p == '-')) negExp = *p++ == '-';
        int exponent = 0, expDigits = 0;
        while (p < end && *p >= '0' && *p <= '9') exponent = std::min(exponent * 10 + (*p++ - '0'), 400), ++expDigits;
        if (expDigits == 0) p = mark;
        else value *= std::pow(10.0, negExp ? -exponent : exponent);
    }
    out = static_cast<float>(negative ? -value : value);
    return std::isfinite(out);
}

Expected<MeshBuilder> ParseTextStl(std::string_view bytes, ImportProgress* progress) {
    MeshBuilder builder(bytes.size() / 250);
    std::array<std::array<float, 3>, 3> corners{};
    int corner = 0;
    std::size_t line = 1;
    const char* p = bytes.data();
    const char* end = p + bytes.size();
    std::size_t step = 0;
    while (p < end) {
        const char* eol = static_cast<const char*>(std::memchr(p, '\n', static_cast<std::size_t>(end - p)));
        if (!eol) eol = end;
        const char* q = p;
        while (q < eol && std::isspace(static_cast<unsigned char>(*q))) ++q;
        if (eol - q >= 6 && std::memcmp(q, "vertex", 6) == 0) {
            q += 6;
            auto& c = corners[static_cast<std::size_t>(corner)];
            if (!ParseNumber(q, eol, c[0]) || !ParseNumber(q, eol, c[1]) || !ParseNumber(q, eol, c[2]))
                return Error{std::format("Текстовый STL повреждён: строка {} — вершина без трёх чисел", line)};
            if (++corner == 3) {
                builder.Add(corners);
                corner = 0;
            }
        } else if (eol - q >= 8 && std::memcmp(q, "endfacet", 8) == 0) {
            if (corner != 0) return Error{std::format("Текстовый STL повреждён: строка {} — у грани не три вершины", line)};
        }
        p = eol + (eol < end ? 1 : 0);
        ++line;
        if (progress && ++step % 65536 == 0) {
            progress->fraction = 0.1 + 0.3 * double(p - bytes.data()) / double(bytes.size());
            if (progress->cancel) return Error{kImportCancelled};
        }
    }
    if (builder.mesh.triangles.empty() && builder.degenerate == 0) return Error{"В текстовом STL нет ни одной грани"};
    return builder;
}

Expected<MeshBuilder> ParseBinaryStl(std::string_view bytes, std::vector<std::string>& warnings, ImportProgress* progress) {
    std::uint32_t count = 0;
    std::memcpy(&count, bytes.data() + 80, 4);
    const std::uint64_t fits = (bytes.size() - 84) / 50;
    if (count > fits) {
        if (fits == 0) return Error{"Двоичный STL обрезан: в нём нет ни одной грани"};
        warnings.push_back(std::format("Файл STL обрезан: заявлено граней {}, есть {} — прочитаны имеющиеся", count, fits));
        count = static_cast<std::uint32_t>(fits);
    }
    MeshBuilder builder(count);
    builder.mesh.triangles.reserve(count);
    const char* p = bytes.data() + 84;
    for (std::uint32_t t = 0; t < count; ++t, p += 50) {
        std::array<std::array<float, 3>, 3> corners;
        std::memcpy(corners.data(), p + 12, 36);
        for (const auto& c : corners)
            for (float v : c)
                if (!std::isfinite(v)) return Error{std::format("Двоичный STL повреждён: грань {} с нечисловыми координатами", t + 1)};
        builder.Add(corners);
        if (progress && t % 65536 == 0) {
            progress->fraction = 0.1 + 0.3 * double(t) / double(count);
            if (progress->cancel) return Error{kImportCancelled};
        }
    }
    return builder;
}

Expected<ImportResult> ImportStl(const std::filesystem::path& path, const ImportOptions& options, ImportProgress* progress) {
    ImportResult result;
    Expected<std::string> bytes = ReadWhole(path, progress, 0.1);
    if (!bytes) return bytes.GetError();
    if (bytes->empty()) return Error{"Файл пуст"};
    if (progress) progress->stage = ImportProgress::Stage::Translating;

    const std::string_view view(*bytes);
    Expected<MeshBuilder> parsed = Error{};
    if (LooksLikeTextStl(view)) {
        parsed = ParseTextStl(view, progress);
    } else if (view.size() >= 84) {
        parsed = ParseBinaryStl(view, result.warnings, progress);
    } else {
        return Error{"Файл не похож на STL: слишком короткий для двоичного и без «solid … facet» текстового"};
    }
    if (!parsed) return parsed.GetError();
    Mesh mesh = std::move(parsed->mesh);
    if (mesh.triangles.empty()) return Error{"В файле STL нет ни одной настоящей грани (все вырождены)"};
    if (parsed->degenerate > 0)
        result.warnings.push_back(std::format("Пропущено вырожденных граней (нулевой площади): {}", parsed->degenerate));

    const std::uint64_t original = mesh.triangles.size();
    if (options.maxTriangles > 0 && original > options.maxTriangles) {
        if (progress) progress->stage = ImportProgress::Stage::Simplifying;
        mesh = Simplify(mesh, options.maxTriangles, progress);
        if (progress && progress->cancel) return Error{kImportCancelled};
        if (mesh.triangles.empty()) return Error{"Не удалось упростить сетку: от неё ничего не осталось"};
        result.warnings.push_back(std::format("Сетка упрощена: {} → {} треугольников", original, mesh.triangles.size()));
    }

    // STL keeps no units: millimetres are assumed. A part smaller than a
    // millimetre was most likely saved in metres (or inches by a CAD in inches).
    std::array<float, 3> lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
    for (const auto& v : mesh.vertices)
        for (std::size_t k = 0; k < 3; ++k) lo[k] = std::min(lo[k], v[k]), hi[k] = std::max(hi[k], v[k]);
    const float largest = std::max({hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]});
    if (largest < 1.0f)
        result.warnings.push_back(std::format("Модель очень мала ({:.3g} мм): в STL нет единиц, возможно, файл сохранён в метрах", largest));

    if (progress) {
        progress->stage = ImportProgress::Stage::Packing;
        progress->fraction = 0.9;
    }
    doc::ImportFeature& f = result.feature;
    f.kind = doc::ImportKind::Mesh;
    f.sourceFormat = "STL";
    f.sourceUnit = "";
    f.bodyCount = 1;
    f.triangles = mesh.triangles.size();
    f.originalTriangles = original;
    std::string block = Pack(kMeshTag, SerializeMesh(mesh));
    if (block.empty()) return Error{"Не удалось упаковать сетку"};
    f.data = std::make_shared<const std::string>(std::move(block));
    return result;
}

// --- STEP reading -----------------------------------------------------------

/// OCCT's progress, passed on to ImportProgress between `from` and `to`;
/// a cancel from the UI stops OCCT's loops.
class Indicator : public Message_ProgressIndicator {
public:
    Indicator(ImportProgress* progress, double from, double to) : myProgress(progress), myFrom(from), myTo(to) {}
    void Show(const Message_ProgressScope&, const Standard_Boolean) override {
        if (myProgress) myProgress->fraction = myFrom + (myTo - myFrom) * GetPosition();
    }
    Standard_Boolean UserBreak() override { return myProgress && myProgress->cancel; }

private:
    ImportProgress* myProgress;
    double          myFrom, myTo;
};

double SolidVolume(const TopoDS_Shape& s) {
    GProp_GProps props;
    BRepGProp::VolumeProperties(s, props);
    return props.Mass();
}

/// Mesh made of every face's triangles: what an AP242 file with tessellated
/// geometry only (no exact surfaces) gives.
Mesh MeshOfFaces(const TopoDS_Shape& shape) {
    MeshBuilder builder(1024);
    for (TopExp_Explorer it(shape, TopAbs_FACE); it.More(); it.Next()) {
        const TopoDS_Face face = TopoDS::Face(it.Current());
        TopLoc_Location location;
        const Handle(Poly_Triangulation) tri = BRep_Tool::Triangulation(face, location);
        if (tri.IsNull()) continue;
        const gp_Trsf trsf = location.Transformation();
        const bool reversed = face.Orientation() == TopAbs_REVERSED;
        for (int t = 1; t <= tri->NbTriangles(); ++t) {
            int n[3];
            tri->Triangle(t).Get(n[0], n[1], n[2]);
            if (reversed) std::swap(n[1], n[2]);
            std::array<std::array<float, 3>, 3> corners;
            for (std::size_t k = 0; k < 3; ++k) {
                const gp_Pnt p = tri->Node(n[k]).Transformed(trsf);
                corners[k] = {float(p.X()), float(p.Y()), float(p.Z())};
            }
            builder.Add(corners);
        }
    }
    return std::move(builder.mesh);
}

/// The bodies of a translated STEP shape: its solids, then closed shells
/// made solid, then the rest of the faces sewn — closed pieces become
/// solids, open ones stay as surfaces.
std::vector<TopoDS_Shape> CollectBodies(const TopoDS_Shape& shape, std::vector<std::string>& warnings, ImportProgress* progress) {
    std::vector<TopoDS_Shape> bodies;
    for (TopExp_Explorer it(shape, TopAbs_SOLID); it.More(); it.Next()) {
        TopoDS_Shape solid = it.Current();
        if (SolidVolume(solid) < 0) solid.Reverse();   // inside-out solid from a careless exporter
        bodies.push_back(solid);
    }
    std::vector<TopoDS_Shape> loose;
    for (TopExp_Explorer it(shape, TopAbs_SHELL, TopAbs_SOLID); it.More(); it.Next()) loose.push_back(it.Current());
    for (TopExp_Explorer it(shape, TopAbs_FACE, TopAbs_SHELL); it.More(); it.Next()) loose.push_back(it.Current());
    if (loose.empty()) return bodies;

    BRepBuilderAPI_Sewing sewing(1e-3);
    for (const TopoDS_Shape& s : loose) sewing.Add(s);
    Handle(Indicator) indicator = new Indicator(progress, 0.85, 0.9);
    sewing.Perform(indicator->Start());
    const TopoDS_Shape sewn = sewing.SewedShape();
    int open = 0;
    const auto take = [&](const TopoDS_Shape& piece) {
        if (piece.ShapeType() == TopAbs_SHELL && BRep_Tool::IsClosed(piece)) {
            BRepBuilderAPI_MakeSolid make(TopoDS::Shell(piece));
            if (make.IsDone()) {
                ShapeFix_Solid fix(make.Solid());
                fix.Perform();
                bodies.push_back(fix.Solid());
                return;
            }
        }
        bodies.push_back(piece);
        ++open;
    };
    if (sewn.ShapeType() == TopAbs_COMPOUND) {
        for (TopoDS_Iterator it(sewn); it.More(); it.Next()) take(it.Value());
    } else {
        take(sewn);
    }
    if (open > 0)
        warnings.push_back(std::format("Незамкнутых поверхностей: {} — они импортированы как поверхности, без объёма", open));
    return bodies;
}

Expected<ImportResult> ImportStep(const std::filesystem::path& path, ImportProgress* progress) {
    ImportResult result;
    STEPControl_Reader reader;
    // Lengths come in millimetres whatever the file is in (inches, metres).
    Interface_Static::SetCVal("xstep.cascade.unit", "MM");
    if (progress) progress->fraction = 0.02;
#if OCC_VERSION_HEX >= 0x070700
    std::ifstream in(path, std::ios::binary);
    if (!in) return Error{std::format("Не удалось открыть файл: {}", ToUtf8(path))};
    const IFSelect_ReturnStatus read = reader.ReadStream("import.step", in);
#else
    const std::string utf8 = ToUtf8(path);
    const IFSelect_ReturnStatus read = reader.ReadFile(utf8.c_str());
#endif
    if (read == IFSelect_RetVoid) return Error{"Файл STEP пуст: в нём нет данных"};
    if (read != IFSelect_RetDone) return Error{"Файл не читается как STEP: он повреждён или это другой формат"};
    if (progress && progress->cancel) return Error{kImportCancelled};

    TColStd_SequenceOfAsciiString lengths, angles, solidAngles;
    reader.FileUnits(lengths, angles, solidAngles);
    if (lengths.Length() > 0) result.feature.sourceUnit = lengths.First().ToCString();

    if (reader.NbRootsForTransfer() == 0) return Error{"В файле STEP нет геометрии (нет ни одной детали или тела)"};
    if (progress) progress->stage = ImportProgress::Stage::Translating;
    Handle(Indicator) translate = new Indicator(progress, 0.3, 0.75);
    reader.TransferRoots(translate->Start());
    if (progress && progress->cancel) return Error{kImportCancelled};
    TopoDS_Shape shape = reader.OneShape();
    if (shape.IsNull()) return Error{"Геометрию из файла STEP перевести не удалось"};

    // Tessellated-only AP242: triangles without surfaces — a mesh body.
    bool exact = false;
    for (TopExp_Explorer it(shape, TopAbs_FACE); it.More() && !exact; it.Next())
        exact = !BRep_Tool::Surface(TopoDS::Face(it.Current())).IsNull();
    if (!exact) {
        Mesh mesh = MeshOfFaces(shape);
        if (mesh.triangles.empty()) return Error{"В файле STEP нет поверхностей — импортировать нечего"};
        result.warnings.push_back("В файле STEP только сетка (без точных поверхностей): импортирована как сетка");
        doc::ImportFeature& f = result.feature;
        f.kind = doc::ImportKind::Mesh;
        f.sourceFormat = "STEP";
        f.bodyCount = 1;
        f.triangles = f.originalTriangles = mesh.triangles.size();
        f.data = std::make_shared<const std::string>(Pack(kMeshTag, SerializeMesh(mesh)));
        return result;
    }

    if (progress) progress->stage = ImportProgress::Stage::Healing;
    Handle(Indicator) heal = new Indicator(progress, 0.75, 0.85);
    ShapeFix_Shape fix(shape);
    fix.SetPrecision(1e-4);
    fix.SetMaxTolerance(0.1);
    fix.Perform(heal->Start());
    if (progress && progress->cancel) return Error{kImportCancelled};
    shape = fix.Shape();

    std::vector<TopoDS_Shape> bodies = CollectBodies(shape, result.warnings, progress);
    if (progress && progress->cancel) return Error{kImportCancelled};
    if (bodies.empty()) return Error{"В файле STEP нет ни тел, ни поверхностей"};
    int invalid = 0;
    for (const TopoDS_Shape& b : bodies)
        if (!BRepCheck_Analyzer(b).IsValid()) ++invalid;
    if (invalid > 0)
        result.warnings.push_back(std::format(
            "Тел с ошибками геометрии после лечения: {} — их можно смотреть и измерять, но операции на них могут не построиться", invalid));

    if (progress) {
        progress->stage = ImportProgress::Stage::Packing;
        progress->fraction = 0.9;
    }
    TopoDS_Compound compound;
    BRep_Builder builder;
    builder.MakeCompound(compound);
    int solids = 0;
    for (const TopoDS_Shape& b : bodies) {
        builder.Add(compound, b);
        if (b.ShapeType() == TopAbs_SOLID) ++solids;
    }
    std::ostringstream stream(std::ios::binary);
    BinTools::Write(compound, stream, false, false, BinTools_FormatVersion_VERSION_3);
    doc::ImportFeature& f = result.feature;
    f.kind = doc::ImportKind::Brep;
    f.sourceFormat = "STEP";
    f.bodyCount = static_cast<int>(bodies.size());
    std::string block = Pack(kBrepTag, stream.str());
    if (block.empty()) return Error{"Не удалось упаковать геометрию"};
    f.data = std::make_shared<const std::string>(std::move(block));
    (void)solids;
    return result;
}

bool HasExtension(const std::filesystem::path& path, std::initializer_list<std::string_view> list) {
    std::string ext = ToUtf8(path.extension());
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return std::find(list.begin(), list.end(), ext) != list.end();
}

} // namespace

// --- Public -----------------------------------------------------------------

Expected<ImportProbe> ProbeImport(const std::filesystem::path& path) {
    std::error_code ec;
    ImportProbe probe;
    probe.bytes = std::filesystem::file_size(path, ec);
    if (ec) return Error{std::format("Файл не найден или недоступен: {}", ToUtf8(path))};
    std::ifstream in(path, std::ios::binary);
    if (!in) return Error{std::format("Не удалось открыть файл: {}", ToUtf8(path))};
    std::string head(std::min<std::uintmax_t>(probe.bytes, 4096), '\0');
    in.read(head.data(), static_cast<std::streamsize>(head.size()));
    head.resize(static_cast<std::size_t>(in.gcount()));

    if (head.find("ISO-10303-21") != std::string::npos) {
        probe.format = ImportFormat::Step;
    } else if (LooksLikeTextStl(head)) {
        probe.format = ImportFormat::Stl;
        probe.triangles = probe.bytes / 250;   // a facet takes ~250 bytes of text
    } else if (head.size() >= 84) {
        std::uint32_t count = 0;
        std::memcpy(&count, head.data() + 80, 4);
        const bool exact = 84 + std::uint64_t{count} * 50 == probe.bytes;
        if (exact || HasExtension(path, {".stl"})) {
            probe.format = ImportFormat::Stl;
            probe.binary = true;
            probe.triangles = exact ? count : (probe.bytes - 84) / 50;
        }
    }
    if (probe.format == ImportFormat::Unknown) {
        if (HasExtension(path, {".step", ".stp"})) return Error{"Файл назван STEP, но не начинается как STEP (ISO-10303-21): он повреждён или это другой формат"};
        if (HasExtension(path, {".stl"})) return Error{"Файл назван STL, но не похож на STL"};
        return Error{"Импорт умеет только STEP (.step, .stp) и STL (.stl)"};
    }
    return probe;
}

bool IsLargeImport(const ImportProbe& probe) {
    return probe.format == ImportFormat::Step ? probe.bytes > kLargeStepBytes : probe.bytes > kLargeStlBytes;
}

Expected<ImportResult> ImportFile(const std::filesystem::path& path, const ImportOptions& options, ImportProgress* progress) {
    const auto start = std::chrono::steady_clock::now();
    Expected<ImportProbe> probe = ProbeImport(path);
    if (!probe) return probe.GetError();
    Expected<ImportResult> result = Error{"Импорт не выполнен"};
    std::string fault;
    // A crash inside OCCT on a broken file costs the import only. The work
    // keeps nothing outside `result` (RunGuarded may skip destructors).
    const bool survived = guard::RunGuarded(
        [&] {
            try {
                result = probe->format == ImportFormat::Step ? ImportStep(path, progress) : ImportStl(path, options, progress);
            } catch (const Standard_Failure& failure) {
                result = Error{std::format("Ошибка геометрического ядра при чтении файла: {}", failure.GetMessageString())};
            } catch (const std::bad_alloc&) {
                result = Error{"Не хватило памяти, чтобы прочитать файл: попробуйте упростить сетку или закрыть другие документы"};
            } catch (const std::exception& e) {
                result = Error{std::format("Внутренняя ошибка при импорте: {}", e.what())};
            }
        },
        &fault);
    if (!survived) {
        log::Error("Import of {}: OCCT fault ({})", ToUtf8(path), fault);
        return Error{"Геометрическое ядро не справилось с файлом (сбой внутри OpenCASCADE): файл, вероятно, повреждён"};
    }
    if (!result) return result;
    result->feature.source = ToUtf8(path);
    result->seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    if (progress) progress->fraction = 1.0;
    log::Info("Import of {}: {} bodies, {:.2f} s", ToUtf8(path), result->feature.bodyCount, result->seconds);
    return result;
}

Expected<ImportedSummary> SummarizeImported(const doc::ImportFeature& feature) {
    Expected<std::vector<TopoDS_Shape>> shapes = occt::DecodeImported(feature);
    if (!shapes) return shapes.GetError();
    ImportedSummary s;
    Bnd_Box box;
    for (const TopoDS_Shape& shape : *shapes) {
        if (occt::IsMesh(shape)) {
            const occt::MeshProperties m = occt::MeasureMesh(shape);
            s.triangles += m.triangles;
            s.volume += m.volume;
            box.Add(m.box);
            continue;
        }
        if (shape.ShapeType() == TopAbs_SOLID) {
            ++s.solids;
            s.volume += SolidVolume(shape);
        } else {
            ++s.shells;
        }
        BRepBndLib::Add(shape, box);
    }
    if (!box.IsVoid()) {
        double x0, y0, z0, x1, y1, z1;
        box.Get(x0, y0, z0, x1, y1, z1);
        s.boxMin = {x0, y0, z0};
        s.boxMax = {x1, y1, z1};
    }
    return s;
}

// --- occt helpers -----------------------------------------------------------

namespace occt {

Expected<std::vector<TopoDS_Shape>> DecodeImported(const doc::ImportFeature& feature) {
    if (!feature.data || feature.data->empty()) return Error{"Импортированное тело пусто"};
    std::string_view tag;
    Expected<std::string> raw = Unpack(*feature.data, tag);
    if (!raw) return raw.GetError();
    std::vector<TopoDS_Shape> out;
    if (tag == kMeshTag) {
        Expected<Mesh> mesh = DeserializeMesh(*raw);
        if (!mesh) return mesh.GetError();
        out.push_back(MeshFace(*mesh));
        return out;
    }
    TopoDS_Shape compound;
    try {
        std::istringstream stream(*raw, std::ios::binary);
        BinTools::Read(compound, stream);
    } catch (const Standard_Failure&) {
        return Error{"Данные импортированного тела повреждены"};
    }
    if (compound.IsNull()) return Error{"Данные импортированного тела повреждены"};
    for (TopoDS_Iterator it(compound); it.More(); it.Next()) out.push_back(it.Value());
    return out;
}

bool IsMesh(const TopoDS_Shape& shape) {
    if (shape.IsNull()) return false;
    bool any = false;
    for (TopExp_Explorer it(shape, TopAbs_FACE); it.More(); it.Next()) {
        const TopoDS_Face face = TopoDS::Face(it.Current());
        if (!BRep_Tool::Surface(face).IsNull()) return false;
        TopLoc_Location location;
        if (BRep_Tool::Triangulation(face, location).IsNull()) return false;
        any = true;
    }
    return any;
}

namespace {
template <class Visit>
void ForEachTriangle(const TopoDS_Shape& mesh, Visit&& visit) {
    for (TopExp_Explorer it(mesh, TopAbs_FACE); it.More(); it.Next()) {
        const TopoDS_Face face = TopoDS::Face(it.Current());
        TopLoc_Location location;
        const Handle(Poly_Triangulation) tri = BRep_Tool::Triangulation(face, location);
        if (tri.IsNull()) continue;
        const gp_Trsf trsf = location.Transformation();
        const bool reversed = face.Orientation() == TopAbs_REVERSED;
        for (int t = 1; t <= tri->NbTriangles(); ++t) {
            int n[3];
            tri->Triangle(t).Get(n[0], n[1], n[2]);
            if (reversed) std::swap(n[1], n[2]);
            visit(tri->Node(n[0]).Transformed(trsf), tri->Node(n[1]).Transformed(trsf), tri->Node(n[2]).Transformed(trsf));
        }
    }
}
} // namespace

MeshProperties MeasureMesh(const TopoDS_Shape& mesh) {
    MeshProperties m;
    gp_XYZ moment(0, 0, 0), areaMoment(0, 0, 0);
    ForEachTriangle(mesh, [&](const gp_Pnt& a, const gp_Pnt& b, const gp_Pnt& c) {
        const double v = a.XYZ().Dot(b.XYZ().Crossed(c.XYZ())) / 6.0;   // signed tetrahedron with the origin
        m.volume += v;
        moment += (a.XYZ() + b.XYZ() + c.XYZ()) * (v / 4.0);
        const double area = gp_Vec(a, b).Crossed(gp_Vec(a, c)).Magnitude() / 2;
        m.area += area;
        areaMoment += (a.XYZ() + b.XYZ() + c.XYZ()) * (area / 3.0);
        m.box.Add(a), m.box.Add(b), m.box.Add(c);
        ++m.triangles;
    });
    if (std::abs(m.volume) > 1e-9) m.centre = gp_Pnt(moment / m.volume);
    else if (m.area > 0) m.centre = gp_Pnt(areaMoment / m.area);
    return m;
}

double MeshDistance(const TopoDS_Shape& mesh, const gp_Pnt& point) {
    double best = 1e300;
    const gp_XYZ p = point.XYZ();
    ForEachTriangle(mesh, [&](const gp_Pnt& pa, const gp_Pnt& pb, const gp_Pnt& pc) {
        // Closest point on a triangle (Ericson, Real-Time Collision Detection 5.1.5).
        const gp_XYZ a = pa.XYZ(), b = pb.XYZ(), c = pc.XYZ();
        const gp_XYZ ab = b - a, ac = c - a, ap = p - a;
        gp_XYZ closest;
        const double d1 = ab.Dot(ap), d2 = ac.Dot(ap);
        const gp_XYZ bp = p - b, cp = p - c;
        const double d3 = ab.Dot(bp), d4 = ac.Dot(bp), d5 = ab.Dot(cp), d6 = ac.Dot(cp);
        const double vc = d1 * d4 - d3 * d2, vb = d5 * d2 - d1 * d6, va = d3 * d6 - d5 * d4;
        if (d1 <= 0 && d2 <= 0) closest = a;
        else if (d3 >= 0 && d4 <= d3) closest = b;
        else if (d6 >= 0 && d5 <= d6) closest = c;
        else if (vc <= 0 && d1 >= 0 && d3 <= 0) closest = a + ab * (d1 / (d1 - d3));
        else if (vb <= 0 && d2 >= 0 && d6 <= 0) closest = a + ac * (d2 / (d2 - d6));
        else if (va <= 0 && d4 - d3 >= 0 && d5 - d6 >= 0) closest = b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
        else {
            const double denom = 1.0 / (va + vb + vc);
            closest = a + ab * (vb * denom) + ac * (vc * denom);
        }
        best = std::min(best, (p - closest).Modulus());
    });
    return best;
}

std::vector<ProjectedCurve> SectionMesh(const TopoDS_Shape& mesh, const Frame& plane) {
    // 1. A segment per triangle the plane crosses (in plane coordinates).
    std::vector<std::array<Vec2, 2>> segments;
    Bnd_Box box;
    const Vec3 n = plane.normal.Normalized();
    ForEachTriangle(mesh, [&](const gp_Pnt& pa, const gp_Pnt& pb, const gp_Pnt& pc) {
        const Vec3 p[3] = {{pa.X(), pa.Y(), pa.Z()}, {pb.X(), pb.Y(), pb.Z()}, {pc.X(), pc.Y(), pc.Z()}};
        double d[3];
        for (int k = 0; k < 3; ++k) {
            d[k] = (p[k] - plane.origin).Dot(n);
            if (d[k] == 0.0) d[k] = 1e-12;   // a corner on the plane counts as above: no double points
        }
        if ((d[0] > 0) == (d[1] > 0) && (d[1] > 0) == (d[2] > 0)) return;
        Vec2 hit[2];
        int found = 0;
        for (int k = 0; k < 3 && found < 2; ++k) {
            const int j = (k + 1) % 3;
            if ((d[k] > 0) == (d[j] > 0)) continue;
            const double t = d[k] / (d[k] - d[j]);
            hit[found++] = plane.ToLocal(p[k] + (p[j] - p[k]) * t);
        }
        if (found == 2) segments.push_back({hit[0], hit[1]});
        box.Add(pa);
    });
    if (segments.empty()) return {};

    // 2. Ends welded on a fine grid, segments chained into polylines.
    double x0, y0, z0, x1, y1, z1;
    box.Get(x0, y0, z0, x1, y1, z1);
    const double size = std::max({x1 - x0, y1 - y0, z1 - z0, 1e-3});
    const double weld = size * 1e-7;
    std::unordered_map<std::uint64_t, std::vector<std::size_t>> at;   // grid key -> segment ends (2·segment + end)
    const auto key = [&](Vec2 v) {
        const auto qx = static_cast<std::int64_t>(std::llround(v.x / weld)), qy = static_cast<std::int64_t>(std::llround(v.y / weld));
        return static_cast<std::uint64_t>(qx) * 0x9E3779B97F4A7C15ull ^ static_cast<std::uint64_t>(qy);
    };
    for (std::size_t i = 0; i < segments.size(); ++i)
        for (std::size_t e = 0; e < 2; ++e) at[key(segments[i][e])].push_back(2 * i + e);
    std::vector<char> used(segments.size(), 0);
    std::vector<std::vector<Vec2>> chains;
    const auto next = [&](Vec2 v, std::size_t self) -> std::optional<std::size_t> {   // an unused end at v
        for (std::size_t end : at[key(v)])
            if (end / 2 != self && !used[end / 2]) return end;
        return std::nullopt;
    };
    for (std::size_t i = 0; i < segments.size(); ++i) {
        if (used[i]) continue;
        used[i] = 1;
        std::vector<Vec2> chain{segments[i][0], segments[i][1]};
        for (int dir = 0; dir < 2; ++dir) {   // grow forward, then backward
            std::size_t last = i;
            while (const auto end = next(chain.back(), last)) {
                const std::size_t s = *end / 2;
                used[s] = 1;
                chain.push_back(segments[s][1 - *end % 2]);
                last = s;
            }
            std::reverse(chain.begin(), chain.end());
        }
        chains.push_back(std::move(chain));
    }

    // 3. Thinned and written out as lines.
    const double tolerance = std::max(0.01, size * 5e-4);
    std::vector<ProjectedCurve> out;
    for (const std::vector<Vec2>& chain : chains) {
        std::vector<char> keep(chain.size(), 0);
        keep.front() = keep.back() = 1;
        std::vector<std::pair<std::size_t, std::size_t>> todo{{0, chain.size() - 1}};
        while (!todo.empty()) {
            const auto [a, b] = todo.back();
            todo.pop_back();
            if (b <= a + 1) continue;
            const Vec2 ab = chain[b] - chain[a];
            const double len = ab.Length();
            double worst = -1;
            std::size_t at2 = a;
            for (std::size_t k = a + 1; k < b; ++k) {
                const Vec2 ak = chain[k] - chain[a];
                const double dist = len > 1e-12 ? std::abs(ab.x * ak.y - ab.y * ak.x) / len : ak.Length();
                if (dist > worst) worst = dist, at2 = k;
            }
            if (worst > tolerance) {
                keep[at2] = 1;
                todo.push_back({a, at2});
                todo.push_back({at2, b});
            }
        }
        std::optional<Vec2> previous;
        for (std::size_t k = 0; k < chain.size(); ++k) {
            if (!keep[k]) continue;
            if (previous && (chain[k] - *previous).Length() > tolerance * 0.1) {
                ProjectedCurve line;
                line.kind = ProjectedCurve::Kind::Line;
                line.start = *previous;
                line.end = chain[k];
                out.push_back(line);
            }
            if (!previous || (chain[k] - *previous).Length() > tolerance * 0.1) previous = chain[k];
        }
    }
    return out;
}

} // namespace occt
} // namespace bz::kernel
