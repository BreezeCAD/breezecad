#pragma once
// Import of STEP (AP203/AP214/AP242) and STL (binary and text) into an
// «Импортированное тело» (doc::ImportFeature). OCCT-free header.
//
// The work is meant for a background thread: ImportFile reads, heals and
// packs the geometry into the feature's data block without touching any
// document or model, reports how far it got through ImportProgress and stops
// when asked to. A fault inside OCCT ends the import with an error, not the
// program (core/FaultGuard). Messages are in Russian (the UI translates).

#include "core/Expected.h"
#include "core/Math.h"
#include "document/ImportFeature.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace bz::kernel {

enum class ImportFormat : unsigned char { Unknown, Step, Stl };

/// What a file is, found quickly (extension, then the first bytes; for STL
/// the triangle count) — before the import starts, so the program can warn
/// about a huge file or offer to simplify a dense mesh.
struct ImportProbe {
    ImportFormat   format = ImportFormat::Unknown;
    std::uintmax_t bytes = 0;
    bool           binary = false;      ///< STL: binary (exact count) or text (estimate)
    std::uint64_t  triangles = 0;       ///< STL only
};

Expected<ImportProbe> ProbeImport(const std::filesystem::path& path);

/// Recommended limits. A mesh above the triangle limit is offered to be
/// simplified; a file above the size limit is warned about before reading.
inline constexpr std::uint64_t  kMeshTriangleLimitDesktop = 500'000;
inline constexpr std::uint64_t  kMeshTriangleLimitPhone = 250'000;
inline constexpr std::uintmax_t kLargeStepBytes = 50ull * 1024 * 1024;
inline constexpr std::uintmax_t kLargeStlBytes = 150ull * 1024 * 1024;

/// Whether the file is large enough to warn about before importing.
bool IsLargeImport(const ImportProbe& probe);

/// Shared between the thread that imports and the one that shows progress.
struct ImportProgress {
    enum class Stage : int { Reading, Translating, Healing, Simplifying, Packing };
    std::atomic<double> fraction{0.0};   ///< 0…1 overall
    std::atomic<Stage>  stage{Stage::Reading};
    std::atomic<bool>   cancel{false};   ///< set by the UI; the import stops soon after
};

struct ImportOptions {
    /// STL: keep at most this many triangles (simplify by vertex clustering);
    /// 0 keeps them all.
    std::uint64_t maxTriangles = 0;
};

struct ImportResult {
    doc::ImportFeature       feature;    ///< ready to add (name and id are the caller's)
    std::vector<std::string> warnings;   ///< healed faces, open shells, odd units...
    double                   seconds = 0.0;
};

/// Error message returned when the user cancelled (ImportProgress::cancel).
inline constexpr const char* kImportCancelled = "Импорт отменён";

/// Reads `path` (STEP or STL by its content) into a feature. Never throws.
Expected<ImportResult> ImportFile(const std::filesystem::path& path, const ImportOptions& options = {},
                                  ImportProgress* progress = nullptr);

/// The bodies an imported feature holds, as they will appear in the part:
/// count and, for a mesh, the triangles. For tests and the tree tooltip.
struct ImportedSummary {
    int           solids = 0;      ///< closed solids
    int           shells = 0;      ///< open surfaces kept as they are
    std::uint64_t triangles = 0;   ///< mesh
    double        volume = 0.0;    ///< mm³ (mesh: of the closed triangle set)
    Vec3          boxMin, boxMax;
};
Expected<ImportedSummary> SummarizeImported(const doc::ImportFeature& feature);

} // namespace bz::kernel
