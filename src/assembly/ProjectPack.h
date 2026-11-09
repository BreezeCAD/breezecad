#pragma once
// A project in one file (.bzp): an assembly with every part it uses, to
// send or carry elsewhere and open there whole.
//
// Inside, a ZIP: the assembly, its parts (where the assembly refers to them
// from its own folder, kept as they were: "Стандартные изделия/Винт.bzc";
// a part from elsewhere goes next to the assembly, renamed if that name is
// taken), and project.json — the "instruction" the program reads first:
//
//   { "format": "bzp", "version": 1, "createdBy": "0.2.5-Beta",
//     "name": "Узел", "main": "Узел.bza",
//     "files": [ { "path": "Узел.bza", "role": "assembly" },
//                { "path": "Кронштейн.bzc", "role": "part", "components": ["Кронштейн:1", "Кронштейн:2"] } ] }
//
// Opening one asks where to put it, unpacks the files there and opens the
// main assembly: from then on it is an ordinary folder with an assembly.

#include "core/Expected.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace bz::assembly {

inline constexpr int kProjectFormatVersion = 1;

struct ProjectFile {
    std::string              path;   ///< inside the archive, '/' separated
    std::string              role;   ///< "assembly" or "part"
    std::vector<std::string> components;   ///< parts: the components that use it
};

struct ProjectManifest {
    std::string              name;
    std::string              main;      ///< the assembly to open
    std::string              createdBy;
    std::vector<ProjectFile> files;
};

/// A part as it went into the archive: where it came from and how the
/// assembly referred to it (to put it back there, see Handoff.h).
struct PackedPart {
    std::string           path;       ///< inside the archive
    std::filesystem::path source;     ///< the file it was read from
    std::string           original;   ///< the component path in the assembly as it was
};

struct PackReport {
    int                      parts = 0;
    std::vector<std::string> missing;   ///< parts the assembly refers to that are not there (left out)
    std::vector<PackedPart>  packed;    ///< every part in the archive, once each
};

/// A path inside an archive that stays inside the folder it is unpacked to
/// (no "..", no drive or root, '/' separated).
bool SafeArchivePath(const std::string& path);

/// Packs the assembly saved at `assemblyFile` with its parts into `out`.
Expected<PackReport> PackProject(const std::filesystem::path& assemblyFile, const std::filesystem::path& out,
                                 std::string_view version);

/// The instruction alone (to show what is inside before unpacking).
Expected<ProjectManifest> ReadProject(const std::filesystem::path& bzp);

/// Unpacks into `folder` (created if needed). A file already there with the
/// same name is not overwritten: an error says which. Returns the main
/// assembly's path.
Expected<std::filesystem::path> UnpackProject(const std::filesystem::path& bzp, const std::filesystem::path& folder);

} // namespace bz::assembly
