#include "document/ImportFeature.h"

#include "document/JsonIO.h"

#include <miniz.h>

#include <array>
#include <format>
#include <utility>

namespace bz::doc {
using json_io::Json;

namespace {

constexpr std::string_view kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

constexpr std::array<signed char, 256> DecodeTable() {
    std::array<signed char, 256> t{};
    for (auto& v : t) v = -1;
    for (std::size_t i = 0; i < kAlphabet.size(); ++i) t[static_cast<unsigned char>(kAlphabet[i])] = static_cast<signed char>(i);
    return t;
}

std::uint32_t Crc32(const std::string& bytes) {
    return static_cast<std::uint32_t>(
        mz_crc32(MZ_CRC32_INIT, reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size()));
}

constexpr std::array<std::pair<ImportKind, std::string_view>, 2> kKinds{{
    {ImportKind::Brep, "brep"},
    {ImportKind::Mesh, "mesh"},
}};

} // namespace

std::string EncodeBase64(std::string_view bytes) {
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 2 < bytes.size(); i += 3) {
        const auto byte = [&](std::size_t k) { return std::uint32_t{static_cast<unsigned char>(bytes[k])}; };
        const std::uint32_t n = (byte(i) << 16) | (byte(i + 1) << 8) | byte(i + 2);
        out += kAlphabet[(n >> 18) & 63];
        out += kAlphabet[(n >> 12) & 63];
        out += kAlphabet[(n >> 6) & 63];
        out += kAlphabet[n & 63];
    }
    if (const std::size_t rest = bytes.size() - i; rest > 0) {
        std::uint32_t n = std::uint32_t{static_cast<unsigned char>(bytes[i])} << 16;
        if (rest == 2) n |= std::uint32_t{static_cast<unsigned char>(bytes[i + 1])} << 8;
        out += kAlphabet[(n >> 18) & 63];
        out += kAlphabet[(n >> 12) & 63];
        out += rest == 2 ? kAlphabet[(n >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

bool DecodeBase64(std::string_view text, std::string& out) {
    static constexpr std::array<signed char, 256> table = DecodeTable();
    if (text.size() % 4 != 0) return false;
    std::string result;
    result.reserve(text.size() / 4 * 3);
    for (std::size_t i = 0; i < text.size(); i += 4) {
        int v[4];
        int pad = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = text[i + static_cast<std::size_t>(k)];
            if (c == '=' && i + 4 == text.size() && k >= 2) {
                v[k] = 0;
                ++pad;
                continue;
            }
            if (pad > 0) return false;   // data after padding
            v[k] = table[static_cast<unsigned char>(c)];
            if (v[k] < 0) return false;
        }
        const std::uint32_t n = (static_cast<std::uint32_t>(v[0]) << 18) | (static_cast<std::uint32_t>(v[1]) << 12) |
                                (static_cast<std::uint32_t>(v[2]) << 6) | static_cast<std::uint32_t>(v[3]);
        result += static_cast<char>((n >> 16) & 0xFF);
        if (pad < 2) result += static_cast<char>((n >> 8) & 0xFF);
        if (pad < 1) result += static_cast<char>(n & 0xFF);
    }
    out = std::move(result);
    return true;
}

void ImportFeature::SaveFields(Json& out) const {
    out["kind"] = std::string(json_io::IdOf(kKinds, kind));
    out["source"] = source;
    out["sourceFormat"] = sourceFormat;
    if (!sourceUnit.empty()) out["sourceUnit"] = sourceUnit;
    out["bodyCount"] = bodyCount;
    if (kind == ImportKind::Mesh) {
        out["triangles"] = triangles;
        out["originalTriangles"] = originalTriangles;
    }
    const std::string& bytes = data ? *data : std::string{};
    out["dataSize"] = bytes.size();
    out["dataCrc32"] = Crc32(bytes);
    out["data"] = EncodeBase64(bytes);
}

Status ImportFeature::LoadFields(const Json& in) {
    ImportFeature f;
    std::string kindId = "brep";
    if (!json_io::ReadMember(in, "kind", kindId) || !json_io::FromId(kKinds, kindId, f.kind))
        return Error{"Импортированное тело: неизвестный вид геометрии"};
    if (!json_io::ReadMember(in, "source", f.source) || !json_io::ReadMember(in, "sourceFormat", f.sourceFormat) ||
        !json_io::ReadMember(in, "sourceUnit", f.sourceUnit) || !json_io::ReadMember(in, "bodyCount", f.bodyCount) ||
        !json_io::ReadMember(in, "triangles", f.triangles) ||
        !json_io::ReadMember(in, "originalTriangles", f.originalTriangles))
        return Error{"Импортированное тело: повреждены сведения об исходном файле"};

    // The geometry itself. A damaged block is an error: the loader then keeps
    // the whole feature verbatim (UnknownFeature), the rest of the part opens.
    std::string text;
    std::uint64_t size = 0;
    std::uint32_t crc = 0;
    std::string bytes;
    if (!in.contains("data") || !json_io::ReadMember(in, "data", text) || !json_io::ReadMember(in, "dataSize", size) ||
        !json_io::ReadMember(in, "dataCrc32", crc) || !DecodeBase64(text, bytes) || bytes.empty() || bytes.size() != size ||
        Crc32(bytes) != crc)
        return Error{"данные импортированного тела повреждены — обновите его из исходного файла"};
    f.data = std::make_shared<const std::string>(std::move(bytes));

    kind = f.kind;
    source = std::move(f.source);
    sourceFormat = std::move(f.sourceFormat);
    sourceUnit = std::move(f.sourceUnit);
    bodyCount = f.bodyCount;
    triangles = f.triangles;
    originalTriangles = f.originalTriangles;
    data = std::move(f.data);
    return Status::Ok();
}

} // namespace bz::doc
