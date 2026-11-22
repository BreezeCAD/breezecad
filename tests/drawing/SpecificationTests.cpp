// The specification of an assembly (ГОСТ 2.106) and the title block filled
// from the model: rows by sections, equal parts counted, positions, pages.
#include "drawing/Drawing.h"
#include "drawing/SheetLayout.h"
#include "drawing/Specification.h"

#include <doctest.h>

#include <algorithm>

using namespace bz;
using namespace bz::drawing;

namespace {

SpecInput Bracket() {
    SpecInput in;
    in.designation = "БРЗ.002.000";
    in.drawingFormat = "А3";
    in.components = {
        {"Кронштейн.bzc", "БРЗ.002.002", "Кронштейн", "", "", 1},
        {"Основание.bzc", "БРЗ.002.001", "Основание", "", "", 1},
        {"Кронштейн.bzc", "БРЗ.002.002", "Кронштейн", "", "", 1},
        {"Стандартные изделия/Болт М8x30.bzc", "Болт М8×30 ГОСТ 7798-70", "Болт", "", "", 4},
        {"Узел/Узел.bza", "БРЗ.002.100", "Узел крепления", "", "", 1},
    };
    return in;
}

const SpecRow* Find(const std::vector<SpecRow>& rows, const std::string& name) {
    const auto it = std::find_if(rows.begin(), rows.end(), [&](const SpecRow& r) { return r.name == name; });
    return it == rows.end() ? nullptr : &*it;
}

} // namespace

TEST_CASE("specification: sections in the order of ГОСТ 2.106, equal parts in one row") {
    const std::vector<SpecRow> rows = SpecRows(Bracket());
    std::vector<std::string> sections;
    for (const SpecRow& r : rows)
        if (r.kind == SpecRow::Kind::Section) sections.push_back(r.name);
    CHECK(sections == std::vector<std::string>{"Документация", "Сборочные единицы", "Детали", "Стандартные изделия"});

    const SpecRow* drawing = Find(rows, "Сборочный чертёж");
    REQUIRE(drawing);
    CHECK(drawing->designation == "БРЗ.002.000 СБ");
    CHECK(drawing->format == "А3");
    CHECK(drawing->position.empty());

    const SpecRow* unit = Find(rows, "Узел крепления");
    const SpecRow* base = Find(rows, "Основание");
    const SpecRow* bracket = Find(rows, "Кронштейн");
    const SpecRow* bolt = Find(rows, "Болт М8×30 ГОСТ 7798-70");
    REQUIRE(unit);
    REQUIRE(base);
    REQUIRE(bracket);
    REQUIRE(bolt);
    // Positions through the sections; parts by designation (…001 before …002).
    CHECK(unit->position == "1");
    CHECK(base->position == "2");
    CHECK(bracket->position == "3");
    CHECK(bolt->position == "4");
    CHECK(bracket->quantity == "2");
    CHECK(bolt->quantity == "4");
    CHECK(bolt->designation.empty());   // a standard part: all in «Наименование»
    // A blank row before and after each section's title.
    const auto title = std::find_if(rows.begin(), rows.end(), [](const SpecRow& r) { return r.name == "Детали"; });
    CHECK((title - 1)->kind == SpecRow::Kind::Blank);
    CHECK((title + 1)->kind == SpecRow::Kind::Blank);
}

TEST_CASE("specification: a position set on the component stays, the rest go round it") {
    SpecInput in = Bracket();
    in.components[1].position = "1";   // «Основание» numbered 1 by the user
    in.components[1].note = "";
    const std::vector<SpecRow> rows = SpecRows(in);
    CHECK(Find(rows, "Основание")->position == "1");
    CHECK(Find(rows, "Узел крепления")->position == "2");
    CHECK(Find(rows, "Кронштейн")->position == "3");
    in.drawingFormat.clear();   // no assembly drawing: no «Документация»
    const std::vector<SpecRow> bare = SpecRows(in);
    CHECK_FALSE(Find(bare, "Документация"));
    CHECK(SpecRows({}).empty());
}

TEST_CASE("specification: pages, form 2 then 2a") {
    CHECK(SpecPageCount(0) == 1);
    CHECK(SpecPageCount(29) == 1);
    CHECK(SpecPageCount(30) == 2);
    CHECK(SpecPageCount(29 + 32) == 2);
    CHECK(SpecPageCount(29 + 33) == 3);

    Sheet sheet;
    sheet.kind = SheetKind::Specification;
    std::vector<SpecRow> rows(40, SpecRow{SpecRow::Kind::Item, "", "", "1", "БРЗ", "Деталь", "1", ""});
    rows[35].name = "Тридцать шестая";
    const SheetLayout first = LayoutSpecPage(sheet, rows, 0, 2);
    const SheetLayout next = LayoutSpecPage(sheet, rows, 1, 2);
    CHECK(first.width == 210);
    CHECK(first.height == 297);
    CHECK(first.titleBlock.max.y - first.titleBlock.min.y == doctest::Approx(40));
    CHECK(next.titleBlock.max.y - next.titleBlock.min.y == doctest::Approx(15));
    const auto has = [](const SheetLayout& l, const std::string& text) {
        return std::any_of(l.labels.begin(), l.labels.end(), [&](const Label& x) { return x.text == text; });
    };
    CHECK(has(first, "Наименование"));
    CHECK_FALSE(has(first, "Тридцать шестая"));   // row 36 is on the second page
    CHECK(has(next, "Тридцать шестая"));
}

TEST_CASE("title block: a field not filled in comes from the model; one filled in wins") {
    Sheet sheet;
    const Fields model{{field::kName, "Вал"}, {field::kMass, "1,25"}, {field::kDesignation, "БРЗ.001"}};
    sheet.title.fields[field::kDesignation] = "БРЗ.999";
    const SheetLayout l = LayoutSheet(sheet, 1, 1, &model);
    const auto value = [&](const char* key) {
        for (const Label& x : l.labels)
            if (x.field == key) return x.text;
        return std::string("—");
    };
    CHECK(value(field::kName) == "Вал");
    CHECK(value(field::kMass) == "1,25");
    CHECK(value(field::kDesignation) == "БРЗ.999");
}

TEST_CASE("pages: drawing sheets and the specification numbered apart; the kind kept in the file") {
    Drawing d;   // «Лист 1»
    Sheet spec;
    spec.id = d.Ids().Next();
    spec.kind = SheetKind::Specification;
    spec.model = ObjectId{42};
    d.sheets.push_back(spec);
    Sheet second;
    second.id = d.Ids().Next();
    d.sheets.push_back(second);
    SheetContent content;
    content.spec = [](const Sheet&) { return std::vector<SpecRow>(40); };
    const std::vector<Page> pages = PagesOf(d, content);
    REQUIRE(pages.size() == 4);
    CHECK(pages[0].number == 1);
    CHECK(pages[0].count == 2);   // two drawing sheets
    CHECK(pages[1].sheet == 1);
    CHECK(pages[2].page == 1);
    CHECK(pages[2].count == 2);   // the specification's own pages
    CHECK(pages[3].number == 2);

    const auto back = FromJson(ToJson(d));
    REQUIRE(back);
    CHECK(back->sheets[1].kind == SheetKind::Specification);
    CHECK(back->sheets[1].model == ObjectId{42});
    CHECK(back->sheets[0].kind == SheetKind::Drawing);
    // A kind of a newer version: kept and written back, shown as a drawing sheet.
    Json j = Json::parse(ToJson(d));
    j["sheets"][2]["kind"] = "bom-table";
    const auto newer = FromJson(j.dump());
    REQUIRE(newer);
    CHECK(newer->sheets[2].kind == SheetKind::Drawing);
    CHECK(Json::parse(ToJson(*newer))["sheets"][2]["kind"] == "bom-table");
}
