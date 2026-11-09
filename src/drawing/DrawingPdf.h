#pragma once
// The drawing as a PDF: a page a sheet, at full size (1:1 in mm), the same
// lines and texts as on the screen (SheetLayout).

#include "core/Expected.h"
#include "drawing/Drawing.h"
#include "drawing/TrueType.h"

#include <cstdint>
#include <filesystem>
#include <string_view>
#include <vector>

namespace bz::drawing {

/// The file's bytes; `producer` — the program and its version, for the
/// document's properties.
std::vector<std::uint8_t> DrawingPdf(const Drawing& drawing, const TrueTypeFont& font, std::string_view producer);

/// Writes it to `target`, through a temporary file next to it.
Status ExportDrawingPdf(const Drawing& drawing, const std::filesystem::path& target, const TrueTypeFont& font,
                        std::string_view producer);

} // namespace bz::drawing
