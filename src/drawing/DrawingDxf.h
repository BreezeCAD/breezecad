#pragma once
// The drawing as DXF (ADR-0012, «Экспорт»: PDF, затем DXF): for CNC and
// other CAD programs. The same lines and texts as the screen and PDF show
// (SheetLayout), at full size in mm, the sheets side by side in one file.
//
// DXF R12 through sheetmetal/DxfDrawing (as the flat pattern). Layers by the
// line of ГОСТ 2.303 (ASCII names: not every CAD program reads others):
// THICK — the main line, THIN — thin, HIDDEN — dashed, AXIS — dash-dot,
// TEXT — texts. Dashes are drawn as pieces of line, as on paper: the file
// needs no line types of its own and shows the same in every program.

#include "core/Expected.h"
#include "drawing/Drawing.h"
#include "drawing/SheetLayout.h"

#include <filesystem>
#include <string>

namespace bz::drawing {

inline constexpr double kDxfSheetGap = 30;   ///< mm between sheets laid side by side

/// The file's text.
std::string DrawingDxf(const Drawing& drawing, const SheetContent& content = {});
/// Writes it to `target`, through a temporary file next to it.
Status ExportDrawingDxf(const Drawing& drawing, const std::filesystem::path& target, const SheetContent& content = {});

} // namespace bz::drawing
