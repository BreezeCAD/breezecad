// A drawing open in the program (ADR-0012, step 2b): undo, «изменён», the
// file written through the session and model paths kept relative to it.

#include "app/Session.h"
#include "drawing/DrawingFile.h"

#include <doctest.h>

#include <filesystem>

using namespace bz;
using namespace bz::app;
namespace fs = std::filesystem;

TEST_CASE("a drawing session: kind, changes, undo and redo") {
    Session s;
    s.MakeDrawing();
    CHECK(s.IsDrawing());
    CHECK(s.DocumentType() == "drawing");
    CHECK_FALSE(s.IsModified());
    CHECK_FALSE(s.CanUndo());
    const auto before = s.ContentRevision();

    s.drawing->Change("Лист и основная надпись", [](drawing::Drawing& d) {
        d.sheets[0].size = drawing::SheetSize::A1;
        d.sheets[0].title.fields[drawing::field::kName] = "Вал";
    });
    CHECK(s.IsModified());
    CHECK(s.ContentRevision() != before);
    REQUIRE(s.CanUndo());
    CHECK(s.UndoLabel() == "Лист и основная надпись");

    s.Undo();
    CHECK(s.drawing->Sheet().size == drawing::SheetSize::A3);
    CHECK(s.drawing->Sheet().title.fields.empty());
    REQUIRE(s.CanRedo());
    s.Redo();
    CHECK(s.drawing->Sheet().size == drawing::SheetSize::A1);
    CHECK(s.drawing->Sheet().title.fields.at(drawing::field::kName) == "Вал");
}

TEST_CASE("the sheet shown follows sheets being removed") {
    DrawingState d;
    const ObjectId first = d.Sheet().id;
    d.Change("Добавить лист", [&](drawing::Drawing& dr) {
        drawing::Sheet second;
        second.id   = dr.Ids().Next();
        second.name = "Лист 2";
        dr.sheets.push_back(second);
        d.activeSheet = second.id;
    });
    CHECK(d.SheetNumber() == 2);
    d.Undo();   // the second sheet is gone: the first one is shown
    CHECK(d.Sheet().id == first);
    CHECK(d.SheetNumber() == 1);
}

TEST_CASE("models: full paths before the first save, relative after it") {
    const fs::path root = fs::temp_directory_path() / "bz_drawing_session";
    fs::remove_all(root);
    fs::create_directories(root / "Детали");
    fs::create_directories(root / "Чертежи");

    Session s;
    s.MakeDrawing();
    s.drawing->drawing.models.push_back({s.drawing->drawing.Ids().Next(), DrawingState::StoredPath(root / "Детали" / "Вал.bzc"), {}});

    // Written to its file: the model relative to it, in the file and in memory.
    const fs::path file = root / "Чертежи" / "Вал.bzd";
    REQUIRE(s.WriteFile(file, "BreezeCAD test"));
    s.path = file;
    s.MarkSaved();
    CHECK_FALSE(s.IsModified());
    CHECK(s.drawing->drawing.models[0].path == "../Детали/Вал.bzc");
    const Expected<drawing::Drawing> saved = drawing::LoadDrawing(file);
    REQUIRE(saved);
    CHECK(saved->models[0].path == "../Детали/Вал.bzc");

    // «Сохранить как» elsewhere: the copy points at the same part.
    const fs::path copy = root / "Вал-копия.bzd";
    REQUIRE(s.WriteFile(copy, "BreezeCAD test"));
    const Expected<drawing::Drawing> copied = drawing::LoadDrawing(copy);
    REQUIRE(copied);
    CHECK(copied->models[0].path == "Детали/Вал.bzc");
    CHECK(s.drawing->drawing.models[0].path == "../Детали/Вал.bzc");   // the open drawing is still its own file's
    fs::remove_all(root);
}
