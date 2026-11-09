#include "drawing/DrawingFile.h"

#include "document/BzcFile.h"
#include "document/DocumentTypes.h"

#include <format>

namespace bz::drawing {

Status SaveDrawing(const Drawing& drawing, const std::filesystem::path& path, std::string_view generator,
                   std::span<const std::uint8_t> preview) {
    return doc::WriteBzcArchive(path, doc::kDrawingType, generator, ToJson(drawing), {}, preview);
}

Expected<Drawing> LoadDrawing(const std::filesystem::path& path, ReadReport* report) {
    Expected<doc::BzcArchive> archive = doc::ReadBzcArchive(path);
    if (!archive) return archive.GetError();
    if (archive->documentType != doc::kDrawingType)
        return Error{std::format("Это не чертёж, а документ типа «{}»", archive->documentType)};
    if (archive->formatVersion > doc::kBzcFormatVersion && report)
        report->warnings.push_back("Файл записан более новой версией BreezeCAD.");
    return FromJson(archive->documentJson, report);
}

} // namespace bz::drawing
