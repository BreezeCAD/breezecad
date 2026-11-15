#pragma once
// The .bzd file: the container of .bzc and .bza (document/BzcFile.h) with
// "documentType": "drawing" in the manifest and drawing.json as its
// document; preview.png is the first sheet for the file manager.

#include "core/Expected.h"
#include "drawing/Drawing.h"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>

namespace bz::drawing {

inline constexpr const char* kDrawingExtension = ".bzd";

/// Writes the drawing atomically (a temporary file renamed over the target).
/// `kernel`: as for doc::WriteBzcArchive — the caller's geometry kernel, for the manifest.
Status SaveDrawing(const Drawing& drawing, const std::filesystem::path& path, std::string_view generator,
                   std::string_view kernel = {}, std::span<const std::uint8_t> preview = {});

/// Reads a .bzd; a file of another kind (a part, an assembly) is an error.
Expected<Drawing> LoadDrawing(const std::filesystem::path& path, ReadReport* report = nullptr);

} // namespace bz::drawing
