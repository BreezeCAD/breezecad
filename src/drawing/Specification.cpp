#include "drawing/Specification.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <tuple>

namespace bz::drawing {

const char* SectionTitle(SpecSection section) {
    switch (section) {
        case SpecSection::Documentation: return "Документация";
        case SpecSection::AssemblyUnits: return "Сборочные единицы";
        case SpecSection::Parts:         return "Детали";
        case SpecSection::Standard:      return "Стандартные изделия";
    }
    return "Детали";
}

SpecSection SectionOf(const SpecComponent& c) {
    std::string ext = c.path.size() >= 4 ? c.path.substr(c.path.size() - 4) : std::string{};
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (ext == ".bza") return SpecSection::AssemblyUnits;
    if (c.path.starts_with("Стандартные изделия/") || c.path.find("/Стандартные изделия/") != std::string::npos) return SpecSection::Standard;
    return SpecSection::Parts;
}

namespace {

struct Group {
    SpecSection   section;
    SpecComponent c;
    int           quantity = 0;
};

using GroupKey = std::tuple<std::string, std::string, std::string, std::string>;
GroupKey KeyOf(const SpecComponent& c) { return std::make_tuple(c.path, c.designation, c.name, c.note); }

/// The rows' components: equal ones together, in the standard's order, numbered.
std::vector<Group> Groups(const SpecInput& input) {
    // Equal components — one file, one designation, name and note — are one row.
    std::map<GroupKey, Group> groups;
    std::vector<GroupKey> order;
    for (const SpecComponent& c : input.components) {
        const auto key = KeyOf(c);
        auto [it, added] = groups.try_emplace(key, Group{SectionOf(c), c, 0});
        if (added) order.push_back(key);
        it->second.quantity += std::max(1, c.quantity);
        if (it->second.c.position.empty()) it->second.c.position = c.position;
    }
    std::vector<Group> items;
    for (const auto& key : order) items.push_back(groups.at(key));
    // ГОСТ 2.106: assembly units and parts by designation, standard parts by name.
    std::stable_sort(items.begin(), items.end(), [](const Group& a, const Group& b) {
        if (a.section != b.section) return a.section < b.section;
        if (a.section == SpecSection::Standard) return a.c.designation < b.c.designation;
        return std::tie(a.c.designation, a.c.name) < std::tie(b.c.designation, b.c.name);
    });

    // Positions: the ones set stay; the others numbered in order around them.
    std::set<int> taken;
    for (const Group& g : items)
        try {
            if (!g.c.position.empty()) taken.insert(std::stoi(g.c.position));
        } catch (...) {
        }
    int next = 1;
    for (Group& g : items) {
        if (!g.c.position.empty()) continue;
        while (taken.count(next)) ++next;
        g.c.position = std::to_string(next++);
    }
    return items;
}

} // namespace

std::vector<std::string> SpecPositions(const SpecInput& input) {
    std::map<GroupKey, std::string> byKey;
    for (const Group& g : Groups(input)) byKey[KeyOf(g.c)] = g.c.position;
    std::vector<std::string> out;
    for (const SpecComponent& c : input.components) out.push_back(byKey[KeyOf(c)]);
    return out;
}

std::vector<SpecRow> SpecRows(const SpecInput& input) {
    const std::vector<Group> items = Groups(input);
    std::vector<SpecRow> rows;
    const auto section = [&](SpecSection s) {
        rows.push_back({});
        rows.push_back({SpecRow::Kind::Section, {}, {}, {}, {}, SectionTitle(s), {}, {}});
        rows.push_back({});
    };
    if (!input.drawingFormat.empty() && !input.designation.empty()) {
        section(SpecSection::Documentation);
        rows.push_back({SpecRow::Kind::Item, input.drawingFormat, {}, {}, input.designation + " СБ", "Сборочный чертёж", {}, {}});
    }
    for (SpecSection s : {SpecSection::AssemblyUnits, SpecSection::Parts, SpecSection::Standard}) {
        bool any = false;
        for (const Group& g : items) {
            if (g.section != s) continue;
            if (!any) section(s);
            any = true;
            SpecRow r{SpecRow::Kind::Item, {}, {}, g.c.position, g.c.designation, g.c.name, std::to_string(g.quantity), g.c.note};
            // A standard part: all of it in «Наименование» (ГОСТ 2.106), no designation.
            if (s == SpecSection::Standard) {
                r.name = g.c.designation.empty() ? g.c.name : g.c.designation;
                r.designation.clear();
            }
            rows.push_back(std::move(r));
        }
    }
    return rows;
}

int SpecPageCount(std::size_t rows) {
    if (rows <= static_cast<std::size_t>(kSpecRowsFirst)) return 1;
    return 1 + static_cast<int>((rows - kSpecRowsFirst + kSpecRowsNext - 1) / kSpecRowsNext);
}

} // namespace bz::drawing
