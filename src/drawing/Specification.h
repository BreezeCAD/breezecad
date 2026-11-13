#pragma once
// The specification of an assembly (ГОСТ 2.106, form 1): its rows made from
// the assembly's components — nothing of it is stored, so it follows the
// assembly as the views do. Sections in the order of the standard, equal
// components in one row with their count, positions numbered through
// (a position or a note set on the component in the assembly is kept).

#include <string>
#include <vector>

namespace bz::drawing {

enum class SpecSection : unsigned char { Documentation, AssemblyUnits, Parts, Standard };
/// «Документация», «Сборочные единицы», «Детали», «Стандартные изделия».
const char* SectionTitle(SpecSection section);

/// A component of the assembly as the specification needs it (the empty
/// fields of the component already filled from its part).
struct SpecComponent {
    std::string path;          ///< the part or sub-assembly file, as the assembly keeps it
    std::string designation;   ///< «БРЗ.001.002»; a standard part: «Болт М8×30 ГОСТ 7798-70»
    std::string name;
    std::string position;      ///< set by the user; empty — numbered here
    std::string note;
    int         quantity = 1;
};

struct SpecInput {
    std::string designation;     ///< of the assembly
    std::string drawingFormat;   ///< «А3»: the format of its assembly drawing; empty — no such row
    std::vector<SpecComponent> components;
};

struct SpecRow {
    enum class Kind : unsigned char { Blank, Section, Item } kind = Kind::Blank;
    std::string format, zone, position, designation, name, quantity, note;
};

/// The section of a component: a sub-assembly (.bza), a standard part (from
/// the folder «Стандартные изделия», as the library puts them), or a part.
SpecSection SectionOf(const SpecComponent& c);
std::vector<SpecRow> SpecRows(const SpecInput& input);
/// The position of each component of `input` (in its order), as the rows number it.
std::vector<std::string> SpecPositions(const SpecInput& input);

/// Rows of the table on the first page (title block form 2) and on the next (form 2a).
inline constexpr int kSpecRowsFirst = 29;
inline constexpr int kSpecRowsNext  = 32;
int SpecPageCount(std::size_t rows);

} // namespace bz::drawing
