#pragma once
// Imported body ("Импортированное тело"): geometry read from a STEP or STL
// file and kept inside the part file itself, so the part moves between a PC
// and a phone without the original file. The source path is remembered only
// for «Обновить из файла».
//
// Unlike every other feature, the input here IS geometry: an opaque block of
// bytes made by kernel/Import (a compressed OCCT BRep or a compressed mesh).
// The document does not look inside it; it only stores it (base64 in the
// JSON) and checks it was not damaged (CRC-32). The block is shared between
// copies of the document (undo, background rebuild): it never changes after
// import, a new import makes a new block.

#include "document/Feature.h"

#include <cstdint>
#include <memory>
#include <string>

namespace bz::doc {

enum class ImportKind : unsigned char {
    Brep,   ///< exact solids from STEP: work like any other body
    Mesh,   ///< triangles from STL: shown, measured, used as a reference
};

class ImportFeature final : public FeatureBase<ImportFeature> {
public:
    static constexpr std::string_view kType = "importedBody";

    ImportKind  kind = ImportKind::Brep;
    std::string source;         ///< UTF-8 path of the original file, for «Обновить из файла»
    std::string sourceFormat;   ///< "STEP" or "STL"
    std::string sourceUnit;     ///< length unit of the file as it said ("INCH", "MM"...), informative
    int         bodyCount = 0;  ///< solids (Brep) or 1 (Mesh), informative
    std::uint64_t triangles = 0;          ///< Mesh: triangles kept
    std::uint64_t originalTriangles = 0;  ///< Mesh: triangles in the file (more when simplified)

    /// Encoded geometry (kernel/Import). Null or empty: nothing imported.
    std::shared_ptr<const std::string> data;
    /// Set on loading when the stored block failed its check; the kernel
    /// then reports the feature broken instead of reading garbage.
    bool damaged = false;

    std::string_view Type() const override { return kType; }
    void SaveFields(nlohmann::json& out) const override;
    Status LoadFields(const nlohmann::json& in) override;
};

/// Base64 (RFC 4648, no line breaks). Exposed for tests.
std::string EncodeBase64(std::string_view bytes);
/// Decodes base64 into `out`; false (and `out` unchanged) when `text` is not valid.
bool DecodeBase64(std::string_view text, std::string& out);

} // namespace bz::doc
