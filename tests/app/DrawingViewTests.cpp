// Views of a part in an open drawing (ADR-0012, step 3), as the program
// runs them but without a window: the standard three put on the sheet, a
// view following its part (a file changed on disk, a part edited in a tab),
// move, scale, delete and undo, the PDF, and the golden file with its part.

#include "../Fixtures.h"
#include "app/Session.h"
#include "app/drawing/DrawingViews.h"
#include "document/BzcFile.h"
#include "drawing/DrawingFile.h"
#include "drawing/DrawingPdf.h"
#include "drawing/ViewLayout.h"

#include <doctest.h>
#include <miniz.h>

#include <cstdlib>
#include <filesystem>
#include <string>

using namespace bz;
using namespace bz::app;
using namespace bz::test;
namespace dw = bz::drawing;
namespace fs = std::filesystem;

namespace {

constexpr double kTol = 0.05;   // mm

doctest::Approx Mm(double expected, double tolerance = kTol) { return doctest::Approx(expected).epsilon(tolerance / expected); }

/// A block 120 x 80 x `height` with a through hole of diameter 20 in the middle.
doc::Document Block(const std::string& height) {
    doc::Document d;
    auto& base = AddSketch(d);
    AddRect(d, base, {-60, -40}, {60, 40});
    AddExtrude(d, base.id, height);
    auto& hole = AddSketch(d);
    AddCircle(d, hole, {0, 0}, 10);
    AddExtrude(d, hole.id, height, doc::BooleanOp::Cut);
    return d;
}

fs::path Folder(const char* name) {
    const fs::path dir = fs::temp_directory_path() / name;
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

/// A drawing session whose only model is the part at `part`.
std::unique_ptr<Session> DrawingOf(const fs::path& part) {
    auto s = std::make_unique<Session>();
    s->MakeDrawing();
    s->drawing->drawing.models.push_back({s->drawing->drawing.Ids().Next(), DrawingState::StoredPath(part), {}});
    return s;
}

const dw::ViewLines& LinesOf(DrawingState& d, ObjectId view) {
    const dw::ViewImages& images = ViewsOf(d).cache.Images();
    const auto it = images.find(view);
    REQUIRE(it != images.end());
    return *it->second;
}

/// Every stream of a PDF, inflated.
std::string Streams(const std::vector<std::uint8_t>& bytes) {
    const std::string pdf(bytes.begin(), bytes.end());
    std::string out;
    for (std::size_t at = pdf.find(">>\nstream\n"); at != std::string::npos; at = pdf.find(">>\nstream\n", at)) {
        at += 10;
        const std::size_t end = pdf.find("\nendstream", at);
        REQUIRE(end != std::string::npos);
        std::string data(64u << 20, '\0');
        mz_ulong size = static_cast<mz_ulong>(data.size());
        if (mz_uncompress(reinterpret_cast<unsigned char*>(data.data()), &size, reinterpret_cast<const unsigned char*>(pdf.data() + at),
                          static_cast<mz_ulong>(end - at)) == MZ_OK)
            out += data.substr(0, size) + "\n";
        at = end;
    }
    return out;
}

std::size_t Count(const std::string& text, const std::string& what) {
    std::size_t count = 0;
    for (std::size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + what.size())) ++count;
    return count;
}

} // namespace

TEST_CASE("standard views: three views of the part on the sheet, one undo step") {
    const fs::path dir = Folder("bz_drawing_views_standard");
    const fs::path part = dir / "Брусок.bzc";
    REQUIRE(doc::SaveBzc(Block("30"), part, "BreezeCAD test"));
    const auto session = DrawingOf(part);
    DrawingState& d = *session->drawing;

    const Expected<ObjectId> main = AddStandardViews(d, nullptr);
    if (!main) FAIL(main.GetError().message);
    const std::vector<dw::ModelView> views = dw::ModelViewsOf(d.Sheet());
    REQUIRE(views.size() == 3);
    CHECK(views[0].id == *main);
    CHECK(views[0].projection == dw::ViewProjection::Front);
    CHECK(views[1].projection == dw::ViewProjection::Top);
    CHECK(views[2].projection == dw::ViewProjection::Left);
    for (const dw::ModelView& v : views) CHECK(v.model == d.drawing.models[0].id);
    CHECK(views[1].parent == *main);
    CHECK(views[2].parent == *main);
    // 120 x 80 x 30 fits an A3 sheet at 1:1; the title block shows the scale.
    REQUIRE(views[0].scale);
    CHECK(*views[0].scale == dw::Scale{1, 1});
    CHECK(d.Sheet().title.fields.at(dw::field::kScale) == "1:1");
    CHECK(d.IsModified());

    // The lines: the main view — the outline and the hole dashed; from above — the circle seen.
    ViewsOf(d).cache.Refresh(d, nullptr);
    CHECK(dw::TotalLength(LinesOf(d, views[0].id).visible) == Mm(2 * (120 + 30)));
    CHECK(dw::TotalLength(LinesOf(d, views[0].id).hidden) == Mm(2 * 30));
    CHECK(dw::TotalLength(LinesOf(d, views[1].id).visible) == Mm(2 * (120 + 80) + 2 * kPi * 10));
    CHECK(dw::TotalLength(LinesOf(d, views[2].id).hidden) == Mm(2 * 30));

    // On the sheet by ГОСТ 2.305 (first angle): from above — under the main view, from the left — to its right.
    const std::vector<dw::PlacedView>& placed = PlacedViews(d);
    REQUIRE(placed.size() == 3);
    for (const dw::PlacedView& p : placed) CHECK(p.computed);
    CHECK(placed[1].box.max.y < placed[0].box.min.y);
    CHECK(placed[2].box.min.x > placed[0].box.max.x);
    CHECK(placed[1].box.min.x == doctest::Approx(placed[0].box.min.x));
    CHECK(placed[1].box.max.x == doctest::Approx(placed[0].box.max.x));
    CHECK(placed[2].box.min.y == doctest::Approx(placed[0].box.min.y));
    CHECK(placed[2].box.max.y == doctest::Approx(placed[0].box.max.y));

    // One step back takes all three away and the scale out of the title block.
    REQUIRE(d.CanUndo());
    CHECK(d.UndoLabel() == "Стандартные виды");
    d.Undo();
    CHECK(dw::ModelViewsOf(d.Sheet()).empty());
    CHECK_FALSE(d.Sheet().title.fields.contains(dw::field::kScale));
    CHECK(PlacedViews(d).empty());
    d.Redo();
    CHECK(dw::ModelViewsOf(d.Sheet()).size() == 3);
    fs::remove_all(dir);
}

TEST_CASE("standard views: why not, in words") {
    Session s;
    s.MakeDrawing();
    DrawingState& d = *s.drawing;
    CHECK_FALSE(AddStandardViews(d, nullptr));   // no model at all
    CHECK_FALSE(d.CanUndo());

    const fs::path dir = Folder("bz_drawing_views_errors");
    d.drawing.models.push_back({d.drawing.Ids().Next(), DrawingState::StoredPath(dir / "Нет такой.bzc"), {}});
    const Expected<ObjectId> missing = AddStandardViews(d, nullptr);
    REQUIRE_FALSE(missing);
    CHECK(missing.GetError().message.find("Нет такой.bzc") != std::string::npos);

    // A part without bodies: nothing to draw.
    REQUIRE(doc::SaveBzc(doc::Document{}, dir / "Пустая.bzc", "BreezeCAD test"));
    d.drawing.models[0].path = DrawingState::StoredPath(dir / "Пустая.bzc");
    CHECK_FALSE(AddStandardViews(d, nullptr));

    // An assembly: its views are a later step.
    d.drawing.models[0].path = DrawingState::StoredPath(dir / "Узел.bza");
    CHECK_FALSE(AddStandardViews(d, nullptr));
    CHECK(dw::ModelViewsOf(d.Sheet()).empty());
    fs::remove_all(dir);
}

TEST_CASE("a view follows its part: the file changed on disk") {
    const fs::path dir = Folder("bz_drawing_views_file");
    const fs::path part = dir / "Брусок.bzc";
    REQUIRE(doc::SaveBzc(Block("30"), part, "BreezeCAD test"));
    const auto session = DrawingOf(part);
    DrawingState& d = *session->drawing;
    const Expected<ObjectId> main = AddStandardViews(d, nullptr);
    REQUIRE(main);
    DrawingViewCache& cache = ViewsOf(d).cache;
    cache.Refresh(d, nullptr);
    CHECK(LinesOf(d, *main).Height() == doctest::Approx(30));
    const std::uint64_t before = cache.Generation();
    cache.Refresh(d, nullptr);
    CHECK(cache.Generation() == before);   // nothing changed: nothing recomputed

    // The part is made taller and saved over the old file.
    doc::Document taller = Block("55");
    {   // a part with one more step, so that the file differs in size as well as in time
        auto& boss = AddSketch(taller);
        AddCircle(taller, boss, {40, 0}, 8);
        AddExtrude(taller, boss.id, "70");
    }
    REQUIRE(doc::SaveBzc(taller, part, "BreezeCAD test"));
    cache.CheckFilesNow();
    cache.Refresh(d, nullptr);
    CHECK(cache.Generation() > before);
    CHECK(LinesOf(d, *main).Height() == doctest::Approx(70));
    // The view kept its place and its scale: only the lines are new.
    CHECK(dw::ModelViewsOf(d.Sheet())[0].id == *main);
    CHECK_FALSE(d.CanRedo());

    // The file is gone: the view stays on the sheet, empty, with a reason; back again — drawn again.
    fs::rename(part, dir / "Спрятан.bzc");
    cache.CheckFilesNow();
    cache.Refresh(d, nullptr);
    CHECK(cache.Images().empty());
    CHECK(cache.Problem(d.drawing.models[0].id).find("Брусок.bzc") != std::string::npos);
    REQUIRE(PlacedViews(d).size() == 3);
    CHECK_FALSE(PlacedViews(d)[0].computed);
    fs::rename(dir / "Спрятан.bzc", part);
    cache.CheckFilesNow();
    cache.Refresh(d, nullptr);
    CHECK(cache.Images().size() == 3);
    CHECK(cache.Problem(d.drawing.models[0].id).empty());
    fs::remove_all(dir);
}

TEST_CASE("a view follows its part: the part is open in a tab and edited there") {
    const fs::path dir = Folder("bz_drawing_views_tab");
    const fs::path part = dir / "Брусок.bzc";
    REQUIRE(doc::SaveBzc(Block("30"), part, "BreezeCAD test"));

    OpenSessions tabs;
    tabs.push_back(std::make_unique<Session>());
    Session& tab = *tabs[0];
    tab.doc = Block("30");
    tab.path = part;
    tab.model.Rebuild(tab.doc);
    tabs.push_back(DrawingOf(part));
    DrawingState& d = *tabs[1]->drawing;

    const Expected<ObjectId> main = AddStandardViews(d, &tabs);
    REQUIRE(main);
    DrawingViewCache& cache = ViewsOf(d).cache;
    cache.Refresh(d, &tabs);
    CHECK(LinesOf(d, *main).Height() == doctest::Approx(30));

    // Edited in its tab and not saved: the drawing shows the part as it is there.
    tab.doc = Block("42");
    tab.model.Rebuild(tab.doc);
    cache.Refresh(d, &tabs);
    CHECK(LinesOf(d, *main).Height() == doctest::Approx(42));

    // The tab is closed: the drawing goes back to the file.
    tabs.erase(tabs.begin());
    cache.Refresh(d, &tabs);
    CHECK(LinesOf(d, *main).Height() == doctest::Approx(30));
    fs::remove_all(dir);
}

TEST_CASE("views: moved along the projection link, scaled together, removed together") {
    const fs::path dir = Folder("bz_drawing_views_edit");
    const fs::path part = dir / "Брусок.bzc";
    REQUIRE(doc::SaveBzc(Block("30"), part, "BreezeCAD test"));
    const auto session = DrawingOf(part);
    DrawingState& d = *session->drawing;
    const Expected<ObjectId> main = AddStandardViews(d, nullptr);
    REQUIRE(main);
    ViewsOf(d).cache.Refresh(d, nullptr);
    const auto place = [&](std::size_t i) { return PlacedViews(d)[i].view.at; };
    const ObjectId top = PlacedViews(d)[1].view.id, left = PlacedViews(d)[2].view.id;
    const Vec2 front0 = place(0), top0 = place(1), left0 = place(2);

    SUBCASE("the main view takes the other two with it") {
        MoveView(d, *main, front0 + Vec2{15, -10});
        CHECK(place(0) == front0 + Vec2{15, -10});
        CHECK(place(1) == top0 + Vec2{15, -10});    // under the main view still, as far from it as before
        CHECK(place(2) == left0 + Vec2{15, -10});   // beside it still
        CHECK(d.UndoLabel() == "Переместить вид");
        d.Undo();
        CHECK(place(0) == front0);
    }
    SUBCASE("a projected view moves along its link only") {
        MoveView(d, top, top0 + Vec2{40, -12});
        CHECK(place(1) == Vec2{top0.x, top0.y - 12});
        MoveView(d, left, left0 + Vec2{25, 30});
        CHECK(place(2) == Vec2{left0.x + 25, left0.y});
    }
    SUBCASE("one scale for the three; the title block follows unless the user wrote his own") {
        SetViewScale(d, top, dw::Scale{1, 2});   // asked on a projected view: its main view is scaled
        for (const dw::PlacedView& p : PlacedViews(d)) CHECK(p.scale == dw::Scale{1, 2});
        CHECK(PlacedViews(d)[0].box.max.x - PlacedViews(d)[0].box.min.x == doctest::Approx(60));
        CHECK(d.Sheet().title.fields.at(dw::field::kScale) == "1:2");
        d.Change("Лист и основная надпись", [](dw::Drawing& dr) { dr.sheets[0].title.fields[dw::field::kScale] = "см. виды"; });
        SetViewScale(d, *main, dw::Scale{2, 1});
        CHECK(PlacedViews(d)[0].scale == dw::Scale{2, 1});
        CHECK(d.Sheet().title.fields.at(dw::field::kScale) == "см. виды");
    }
    SUBCASE("hidden lines can be switched off for one view") {
        const auto thin = [&](std::size_t i) {
            return std::count_if(PlacedViews(d)[i].paths.begin(), PlacedViews(d)[i].paths.end(),
                                 [](const dw::ViewPath& p) { return p.weight == dw::LineWeight::Thin; });
        };
        CHECK(thin(0) > 0);
        SetViewHiddenLines(d, *main, false);
        CHECK(thin(0) == 0);
        CHECK(thin(2) > 0);   // the view from the left keeps its own
    }
    SUBCASE("removing a projected view leaves the rest; removing the main one takes its projections") {
        ViewsOf(d).selected = left;
        RemoveView(d, left);
        CHECK(dw::ModelViewsOf(d.Sheet()).size() == 2);
        CHECK_FALSE(ViewsOf(d).selected.IsValid());
        RemoveView(d, *main);
        CHECK(dw::ModelViewsOf(d.Sheet()).empty());
        d.Undo();
        d.Undo();
        CHECK(dw::ModelViewsOf(d.Sheet()).size() == 3);
    }
    fs::remove_all(dir);
}

TEST_CASE("views: saved with the drawing and drawn again after opening; in the PDF with both pens") {
    const fs::path dir = Folder("bz_drawing_views_save");
    const fs::path part = dir / "Брусок.bzc";
    REQUIRE(doc::SaveBzc(Block("30"), part, "BreezeCAD test"));
    const auto session = DrawingOf(part);
    REQUIRE(AddStandardViews(*session->drawing, nullptr));
    const fs::path file = dir / "Брусок.bzd";
    REQUIRE(session->WriteFile(file, "BreezeCAD test"));
    session->path = file;
    session->MarkSaved();
    CHECK(session->drawing->drawing.models[0].path == "Брусок.bzc");   // beside the drawing now
    CHECK(DrawingViewImages(*session->drawing, nullptr).size() == 3);  // and still found there

    dw::ReadReport report;
    Expected<dw::Drawing> loaded = dw::LoadDrawing(file, &report);
    REQUIRE(loaded);
    CHECK(report.warnings.empty());
    DrawingState opened;
    opened.drawing = std::move(*loaded);
    opened.savedAt = file;
    const dw::ViewImages& images = DrawingViewImages(opened, nullptr);
    CHECK(images.size() == 3);

    const Expected<dw::TrueTypeFont> font = dw::TrueTypeFont::FromFile(fs::path(BZ_SOURCE_DIR) / "assets" / "fonts" / "IBMPlexSans-Regular.ttf");
    REQUIRE(font);
    const std::string bare = Streams(dw::DrawingPdf(opened.drawing, *font, "BreezeCAD test"));
    const std::string full = Streams(dw::DrawingPdf(opened.drawing, *font, "BreezeCAD test", &images));
    // The frame alone has both pens already; the views add solid main lines and thin dashes.
    CHECK(Count(full, "0.5 w ") > Count(bare, "0.5 w ") + 60);     // three outlines and the circle
    CHECK(Count(full, "0.18 w ") >= Count(bare, "0.18 w ") + 4 * 5);   // four hidden lines of 30 mm, dashes of about 4
    fs::remove_all(dir);
}

TEST_CASE("the golden drawing with views and its part: opened and drawn as when it was written") {
    // tests/data/drawings/v1/a3-brusok-views.bzd with brusok.bzc beside it: written by the
    // version that added views. Never rewrite them (ADR-0012). BZ_WRITE_DRAWING_GOLDEN=1
    // made the pair once.
    const fs::path dir = fs::path(BZ_SOURCE_DIR) / "tests" / "data" / "drawings" / "v1";
    const fs::path file = dir / "a3-brusok-views.bzd", part = dir / "brusok.bzc";
    if (std::getenv("BZ_WRITE_DRAWING_GOLDEN") && !fs::exists(file)) {
        doc::Document block = Block("30");
        block.info.name = "Брусок";
        block.info.designation = "БРЗ.001.003";
        REQUIRE(doc::SaveBzc(block, part, "BreezeCAD 0.2.5.1-Beta"));
        const auto session = DrawingOf(part);
        session->drawing->Sheet().title.fields[dw::field::kName] = "Брусок";
        session->drawing->Sheet().title.fields[dw::field::kDesignation] = "БРЗ.001.003";
        REQUIRE(AddStandardViews(*session->drawing, nullptr));
        REQUIRE(session->WriteFile(file, "BreezeCAD 0.2.5.1-Beta"));
    }
    Expected<dw::Drawing> loaded = dw::LoadDrawing(file);
    if (!loaded) FAIL(loaded.GetError().message);
    DrawingState d;
    d.drawing = std::move(*loaded);
    d.savedAt = file;
    const dw::ViewImages& images = DrawingViewImages(d, nullptr);
    const std::vector<dw::PlacedView>& placed = PlacedViews(d);
    REQUIRE(placed.size() == 3);
    for (const dw::PlacedView& p : placed) CHECK(p.computed);
    REQUIRE(images.size() == 3);
    CHECK(dw::TotalLength(images.at(placed[0].view.id)->visible) == Mm(2 * (120 + 30)));
    CHECK(dw::TotalLength(images.at(placed[0].view.id)->hidden) == Mm(2 * 30));
    CHECK(dw::TotalLength(images.at(placed[1].view.id)->visible) == Mm(2 * (120 + 80) + 2 * kPi * 10));
    // Where the views stood when the file was written.
    CHECK(placed[0].box.min.x == doctest::Approx(107.5));
    CHECK(placed[0].box.max.y == doctest::Approx(213.5));
    CHECK(placed[1].box.max.y == doctest::Approx(163.5));
    CHECK(placed[2].box.min.x == doctest::Approx(247.5));
}
