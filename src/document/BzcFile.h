#pragma once
// The .bzc file: a ZIP archive with JSON inside. Specification:
// docs/FILE_FORMAT.md. The recipe (features and their inputs) is the source
// of truth; anything computed is only a cache and may be absent.

#include "core/Expected.h"
#include "document/Document.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bz::doc {

inline constexpr int kBzcFormatVersion = 1;

struct LoadReport {
    /// Non-fatal problems: unknown feature types, newer format version...
    /// Shown to the user after opening.
    std::vector<std::string> warnings;
};

/// The container shared by every kind of document (DocumentTypes.h): a
/// manifest naming the kind, the document as JSON and its pictures.
struct BzcArchive {
    std::string documentType = "part";   ///< files without it are parts
    int         formatVersion = 0;
    std::string kernel;   ///< the geometry kernel that wrote it ("OpenCASCADE 8.0.1"); empty in files before 0.2.6
    std::string documentJson;
    std::map<std::string, std::vector<std::uint8_t>> pictures;   ///< by name, without "pictures/"
};

/// Reads the container; does not look inside the document.
Expected<BzcArchive> ReadBzcArchive(const std::filesystem::path& path);
/// Writes the container atomically (temporary file, then rename).
/// `kernel`: the geometry kernel of the program that writes ("OpenCASCADE
/// 8.0.1"), for manifest.json — document/ does not know OCCT, the caller
/// passes it as it passes `generator`. Empty: no "kernel" in the manifest.
Status WriteBzcArchive(const std::filesystem::path& path, std::string_view documentType, std::string_view generator,
                       std::string_view kernel, const std::string& documentJson,
                       const std::map<std::string, std::vector<std::uint8_t>>& pictures = {},
                       std::span<const std::uint8_t> preview = {});
/// The picture of the model kept in the file (PNG with transparency) for the
/// file manager's icons. Without a new one, WriteBzcArchive keeps the old.
Expected<std::vector<std::uint8_t>> ReadBzcPreview(const std::filesystem::path& path);
/// The kind of document in the file ("part", "assembly"...).
Expected<std::string> ReadDocumentType(const std::filesystem::path& path);

/// Writes the document atomically: a temporary file is written next to the
/// target and renamed over it, so a crash never leaves a half-written file.
Status SaveBzc(const Document& doc, const std::filesystem::path& path, std::string_view generator,
               std::string_view kernel = {}, std::span<const std::uint8_t> preview = {});

Expected<Document> LoadBzc(const std::filesystem::path& path, LoadReport* report = nullptr);

/// The document part of the archive as JSON text (also used by tests).
std::string        DocumentToJson(const Document& doc);
Expected<Document> DocumentFromJson(std::string_view json, LoadReport* report = nullptr);

} // namespace bz::doc
