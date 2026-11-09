#pragma once
// A drawing (ADR-0012): sheets with a frame and a title block by ESKD, the
// models its views are made from, and the views and annotations themselves.
// Only what the user chose is kept; every line of a view is computed from
// the model, so a changed part updates the drawing.
//
// The format only grows (ADR-0012, «Обратная совместимость»): a key once
// written keeps its name and meaning; what this version does not know —
// a view or an annotation type, a key of a known object — is read as it is
// and written back unchanged. A file of an older format version is moved
// to the current one by Upgrade() when it is read.

#include "core/Expected.h"
#include "core/Id.h"

#include <nlohmann/json.hpp>

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace bz::drawing {

using Json = nlohmann::json;

/// The version of drawing.json this build writes. Raise it only when an
/// older program would read a file wrongly (not when it merely misses
/// something new), and add the step to Upgrade().
inline constexpr int kDrawingFormatVersion = 1;

/// Sheet sizes of ГОСТ 2.301 (ISO 216 A series).
enum class SheetSize : unsigned char { A4, A3, A2, A1, A0 };
enum class Orientation : unsigned char { Portrait, Landscape };

/// A model the drawing is made from: a part or an assembly, by a path
/// relative to the drawing (as an assembly keeps its parts).
struct ModelRef {
    ObjectId    id;
    std::string path;   ///< "Вал.bzc", "../Узел/Узел.bza"; '/' between folders
    Json        extra = Json::object();   ///< keys of a newer version, written back as they were
};

/// A view or an annotation as the file holds it. This version has no types
/// of its own yet (views come with step 3 of ADR-0012): every one is kept
/// whole and written back; the editor shows it as «из более новой версии».
struct Item {
    Json json;   ///< the object as read, "type" included
    std::string Type() const;
};

/// Fields of the title block (ГОСТ 2.104, form 1) by their keys below. A
/// field not set takes its value from the model or the sheet (name,
/// designation, material, mass, scale, sheet numbers); an empty one set by
/// the user stays empty.
struct TitleBlock {
    std::map<std::string, std::string> fields;
    Json extra = Json::object();
};

/// Keys of the title block fields: stable, ASCII, written in files.
namespace field {
inline constexpr const char* kName         = "name";          ///< графа 1: наименование
inline constexpr const char* kDesignation  = "designation";   ///< графа 2: обозначение
inline constexpr const char* kMaterial     = "material";      ///< графа 3: материал
inline constexpr const char* kLetter       = "letter";        ///< графа 4: литера
inline constexpr const char* kMass         = "mass";          ///< графа 5: масса
inline constexpr const char* kScale        = "scale";         ///< графа 6: масштаб
inline constexpr const char* kOrganization = "organization";  ///< графа 9: организация
inline constexpr const char* kDeveloper    = "developer";     ///< графа 11: Разраб.
inline constexpr const char* kChecker      = "checker";       ///< Пров.
inline constexpr const char* kTechControl  = "techControl";   ///< Т.контр.
inline constexpr const char* kNormControl  = "normControl";   ///< Н.контр.
inline constexpr const char* kApprover     = "approver";      ///< Утв.
} // namespace field

struct Sheet {
    ObjectId    id;
    std::string name;   ///< «Лист 1»
    SheetSize   size = SheetSize::A3;
    Orientation orientation = Orientation::Landscape;
    TitleBlock  title;
    std::vector<Item> views;         ///< in the order drawn
    std::vector<Item> annotations;   ///< dimensions, notes… (step 5 on)
    Json extra = Json::object();
};

class Drawing {
public:
    Drawing();   ///< one A3 landscape sheet, «Лист 1»

    std::vector<ModelRef> models;
    std::vector<Sheet>    sheets;
    Json extra = Json::object();   ///< top-level keys of a newer version

    IdGenerator&       Ids() { return myIds; }
    const IdGenerator& Ids() const { return myIds; }

private:
    IdGenerator myIds;
};

/// What reading a file noticed (shown to the user after opening).
struct ReadReport {
    std::vector<std::string> warnings;
};

/// drawing.json as text.
std::string ToJson(const Drawing& drawing);
/// From drawing.json of any format version up to the current one; a newer
/// one is read too, with a warning (its unknown parts are kept).
Expected<Drawing> FromJson(std::string_view text, ReadReport* report = nullptr);

/// Brings drawing.json of format `version` to the current one, step by step.
/// Each step is a function with its own test; version 1 needs none yet.
Expected<Json> Upgrade(Json root, int version);

/// Short names used in files: "A3", "landscape"…
std::string_view SizeId(SheetSize size);
std::string_view OrientationId(Orientation orientation);

} // namespace bz::drawing
