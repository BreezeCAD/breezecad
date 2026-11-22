#pragma once
// The drawing as a PDF: a page a sheet, at full size (1:1 in mm), the same
// lines and texts as on the screen (SheetLayout).

#include "core/Expected.h"
#include "drawing/Drawing.h"
#include "drawing/SheetLayout.h"
#include "drawing/TrueType.h"

#include <cstdint>
#include <filesystem>
#include <string_view>
#include <vector>

namespace bz::drawing {

/// The file's bytes; `producer` — the program and its version, for the
/// document's properties; `content` — views, model fields, specification rows
/// (none: frames only). A page a drawing sheet, a specification as many as it takes.
std::vector<std::uint8_t> DrawingPdf(const Drawing& drawing, const TrueTypeFont& font, std::string_view producer,
                                     const SheetContent& content = {});

/// Writes it to `target`, through a temporary file next to it.
Status ExportDrawingPdf(const Drawing& drawing, const std::filesystem::path& target, const TrueTypeFont& font,
                        std::string_view producer, const SheetContent& content = {});

/// ГОСТ 2.303 dashes on paper, mm: strokes and gaps of a style (empty: solid).
std::vector<double> DashOf(LineStyle style);

} // namespace bz::drawing
