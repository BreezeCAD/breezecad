// Views of a model in a drawing (ADR-0012, step 3) without the kernel: how a
// view is kept in the file and the compatibility rules for it, the
// projection link, the dashed line of ГОСТ 2.303, the first arrangement of
// the standard views, cleaning of overlapping lines, and the PDF.

#include "drawing/DrawingFile.h"
#include "drawing/DrawingPdf.h"
#include "drawing/ModelView.h"
#include "drawing/ViewLayout.h"
#include "drawing/ViewLines.h"

#include <doctest.h>
#include <miniz.h>

#include <cmath>
#include <filesystem>
#include <memory>
#include <string>

using namespace bz;
using namespace bz::drawing;
namespace fs = std::filesystem;

namespace {

fs::path Golden(const char* name) { return fs::path(BZ_SOURCE_DIR) / "tests" / "data" / "drawings" / "v1" / name; }

Polyline Rect(Vec2 lo, Vec2 hi) { return {lo, {hi.x, lo.y}, hi, {lo.x, hi.y}, lo}; }

/// The lines of a box `width` x `height` standing on the origin's level, with
/// a hidden vertical line in its middle.
std::shared_ptr<const ViewLines> BoxLines(double width, double height) {
    ViewLines lines;
    lines.visible.push_back(Rect({-width / 2, 0}, {width / 2, height}));
    lines.hidden.push_back({{0, 0}, {0, height}});
    RemoveOverlaps(lines, 1e-3);
    return std::make_shared<const ViewLines>(std::move(lines));
}

double PathLength(const ViewPath& path) { return TotalLength({path.points}); }

/// Every stream of a PDF, inflated.
std::string Streams(const std::string& pdf) {
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

/// A sheet with the three standard views of the model 2: ids 10 (main), 11 (above), 12 (left).
Drawing ThreeViews() {
    Drawing d;
    d.models.push_back({d.Ids().Next(), "Брусок.bzc", Json::object()});
    d.Ids().Reserve(ObjectId{12});
    ModelView front{ObjectId{10}, d.models[0].id, ViewProjection::Front, Scale{1, 2}, {120, 200}, {}, true};
    ModelView top{ObjectId{11}, d.models[0].id, ViewProjection::Top, std::nullopt, {0, 120}, ObjectId{10}, true};
    ModelView left{ObjectId{12}, d.models[0].id, ViewProjection::Left, std::nullopt, {250, 0}, ObjectId{10}, true};
    for (const ModelView& v : {front, top, left}) d.sheets[0].views.push_back(MakeViewItem(v));
    return d;
}

} // namespace

TEST_CASE("model view: kept in the file by stable keys and read back") {
    const Drawing d = ThreeViews();
    const Json saved = Json::parse(ToJson(d));
    const Json& views = saved["sheets"][0]["views"];
    REQUIRE(views.size() == 3);
    // The keys are the format: a name once written is never changed (ADR-0012).
    CHECK(views[0] == Json::parse(R"({"type": "modelView", "id": 10, "model": 2, "projection": "front",
                                      "scale": [1.0, 2.0], "at": [120.0, 200.0], "hiddenLines": true})"));
    CHECK(views[1] == Json::parse(R"({"type": "modelView", "id": 11, "model": 2, "projection": "top", "parent": 10,
                                      "at": [0.0, 120.0], "hiddenLines": true})"));
    CHECK(views[2]["projection"] == "left");
    CHECK(saved["formatVersion"] == 1);   // nothing an older program would read wrongly

    ReadReport report;
    const Expected<Drawing> back = FromJson(ToJson(d), &report);
    REQUIRE(back);
    CHECK(report.warnings.empty());   // views of this kind are known now
    const std::vector<ModelView> read = ModelViewsOf(back->sheets[0]);
    REQUIRE(read.size() == 3);
    CHECK(read[0].id == ObjectId{10});
    CHECK(read[0].projection == ViewProjection::Front);
    REQUIRE(read[0].scale);
    CHECK(*read[0].scale == Scale{1, 2});
    CHECK(read[0].at == Vec2{120, 200});
    CHECK_FALSE(read[0].parent.IsValid());
    CHECK(read[1].parent == ObjectId{10});
    CHECK_FALSE(read[1].scale);
    CHECK(read[2].projection == ViewProjection::Left);
    CHECK(UnknownItemCount(*back) == 0);
    // New ids go after the views' ones.
    Drawing copy = *back;
    CHECK(copy.Ids().Next().value > 12);
}

TEST_CASE("model view: keys and projections of a newer version are kept") {
    // A view of this kind with a key added later, a projection this build does not
    // draw, and a view of another kind altogether.
    const char* future = R"({
        "formatVersion": 1, "nextId": 30,
        "models": [{"id": 2, "path": "Вал.bzc"}],
        "sheets": [{"id": 3, "name": "Лист 1", "size": "A3", "orientation": "landscape",
            "views": [
                {"type": "modelView", "id": 10, "model": 2, "projection": "front", "scale": [1, 1], "at": [100, 150],
                 "hiddenLines": false, "tangentEdges": true, "label": {"text": "А"}},
                {"type": "modelView", "id": 11, "model": 2, "projection": "isometric", "scale": [1, 1], "at": [300, 200]},
                {"type": "section", "id": 12, "parent": 10, "letter": "А"}
            ]}]
    })";
    ReadReport report;
    Expected<Drawing> d = FromJson(future, &report);
    REQUIRE(d);
    Sheet& sheet = d->sheets[0];
    const std::vector<ModelView> known = ModelViewsOf(sheet);
    REQUIRE(known.size() == 1);
    CHECK_FALSE(known[0].hiddenLines);
    CHECK(UnknownItemCount(sheet) == 2);
    CHECK(report.warnings.size() == 1);   // «…виды, которые эта версия не показывает»

    // The known view is changed here; what the newer version put into it stays.
    ModelView moved = known[0];
    moved.at = {110, 160};
    moved.scale = Scale{1, 2};
    Item* item = FindViewItem(sheet, moved.id);
    REQUIRE(item);
    StoreModelView(*item, moved);

    const Json saved = Json::parse(ToJson(*d));
    const Json& views = saved["sheets"][0]["views"];
    const Json before = Json::parse(future)["sheets"][0]["views"];
    REQUIRE(views.size() == 3);
    CHECK(views[0]["tangentEdges"] == true);
    CHECK(views[0]["label"]["text"] == "А");
    CHECK(views[0]["at"] == Json::array({110.0, 160.0}));
    CHECK(views[0]["scale"] == Json::array({1.0, 2.0}));
    CHECK(views[1] == before[1]);   // untouched, in its place
    CHECK(views[2] == before[2]);
}

TEST_CASE("model view: odd values do not break reading") {
    const auto read = [](const char* json) { return ReadModelView(Item{Json::parse(json)}); };
    CHECK_FALSE(read(R"({"type": "modelView"})"));                                           // no id, no model
    CHECK_FALSE(read(R"({"type": "modelView", "id": 0, "model": 2, "projection": "front"})"));   // 0 is not an id
    CHECK_FALSE(read(R"({"type": "modelView", "id": 5, "model": 2})"));                       // no projection
    CHECK_FALSE(read(R"({"type": "linear", "id": 5, "model": 2, "projection": "front"})"));
    const auto odd = read(R"({"type": "modelView", "id": 5, "model": 2, "projection": "top",
                              "scale": [0, 1], "at": "middle", "parent": -3, "hiddenLines": "yes"})");
    REQUIRE(odd);   // the view is there; what cannot be read takes its default
    CHECK_FALSE(odd->scale);
    CHECK(odd->at == Vec2{});
    CHECK_FALSE(odd->parent.IsValid());
    CHECK(odd->hiddenLines);
}

TEST_CASE("model view: the golden file with views opens forever") {
    // Written by the version that added views (step 3). Never rewrite it (ADR-0012).
    ReadReport report;
    const Expected<Drawing> d = LoadDrawing(Golden("a3-brusok-views.bzd"), &report);
    if (!d) FAIL(d.GetError().message);
    CHECK(report.warnings.empty());
    REQUIRE(d->models.size() == 1);
    CHECK(d->models[0].path == "brusok.bzc");
    REQUIRE(d->sheets.size() == 1);
    const std::vector<ModelView> views = ModelViewsOf(d->sheets[0]);
    REQUIRE(views.size() == 3);
    CHECK(views[0].projection == ViewProjection::Front);
    CHECK(views[1].projection == ViewProjection::Top);
    CHECK(views[2].projection == ViewProjection::Left);
    CHECK(views[0].model == d->models[0].id);
    REQUIRE(views[0].scale);
    CHECK(*views[0].scale == Scale{1, 1});
    CHECK(views[1].parent == views[0].id);
    CHECK(views[2].parent == views[0].id);
    CHECK(views[0].hiddenLines);
    CHECK(d->sheets[0].title.fields.at(field::kScale) == "1:1");
}

TEST_CASE("scales of ГОСТ 2.302 as written") {
    CHECK(ScaleText({1, 1}) == "1:1");
    CHECK(ScaleText({1, 2.5}) == "1:2,5");
    CHECK(ScaleText({2.5, 1}) == "2,5:1");
    CHECK(ScaleText({1, 1000}) == "1:1000");
    const auto scales = StandardScales();
    for (std::size_t i = 1; i < scales.size(); ++i) CHECK(scales[i].Factor() < scales[i - 1].Factor());
    CHECK(scales.front() == Scale{100, 1});
    CHECK(scales.back() == Scale{1, 1000});
}

TEST_CASE("projection link: a view from above keeps under the main one, a view from the left beside it") {
    Drawing d = ThreeViews();
    const ViewImages images{{ObjectId{10}, BoxLines(120, 30)}, {ObjectId{11}, BoxLines(120, 80)}, {ObjectId{12}, BoxLines(80, 30)}};
    std::vector<PlacedView> placed = LayoutViews(d.sheets[0], images);
    REQUIRE(placed.size() == 3);
    CHECK(placed[0].view.at == Vec2{120, 200});
    CHECK(placed[1].view.at == Vec2{120, 120});   // x from the main view, its own y
    CHECK(placed[2].view.at == Vec2{250, 200});   // its own x, y from the main view
    for (const PlacedView& p : placed) CHECK(p.scale == Scale{1, 2});   // one scale for views in projection
    // 1:2 — the box of the main view is 60 × 15 on the sheet, standing on y = 200.
    CHECK(placed[0].box.min.x == doctest::Approx(90));
    CHECK(placed[0].box.max.x == doctest::Approx(150));
    CHECK(placed[0].box.min.y == doctest::Approx(200));
    CHECK(placed[0].box.max.y == doctest::Approx(215));

    // The main view moves: the other two go with it, each as far from it as before.
    MoveViewTo(d.sheets[0], ObjectId{10}, {140, 180});
    placed = LayoutViews(d.sheets[0], images);
    CHECK(placed[0].view.at == Vec2{140, 180});
    CHECK(placed[1].view.at == Vec2{140, 100});
    CHECK(placed[2].view.at == Vec2{270, 180});
    // A projected view moves along its link only; the main view stays.
    MoveViewTo(d.sheets[0], ObjectId{11}, {300, 60});
    MoveViewTo(d.sheets[0], ObjectId{12}, {290, 10});
    placed = LayoutViews(d.sheets[0], images);
    CHECK(placed[0].view.at == Vec2{140, 180});
    CHECK(placed[1].view.at == Vec2{140, 60});
    CHECK(placed[2].view.at == Vec2{290, 180});

    // Without its main view a projected one stands by itself, at 1:1.
    d.sheets[0].views.erase(d.sheets[0].views.begin());
    placed = LayoutViews(d.sheets[0], images);
    REQUIRE(placed.size() == 2);
    CHECK(placed[0].view.at == Vec2{140, 60});
    CHECK(placed[0].scale == Scale{1, 1});
}

TEST_CASE("a view on the sheet: seen lines thick and whole, hidden ones thin and dashed") {
    Drawing d = ThreeViews();
    const ViewImages images{{ObjectId{10}, BoxLines(120, 60)}};
    std::vector<PlacedView> placed = LayoutViews(d.sheets[0], images);
    const PlacedView& front = placed[0];
    REQUIRE(front.computed);
    double thick = 0, thin = 0;
    std::size_t dashes = 0;
    for (const ViewPath& path : front.paths) {
        if (path.weight == LineWeight::Thick) thick += PathLength(path);
        else {
            thin += PathLength(path);
            ++dashes;
            // ГОСТ 2.303: a dash is 2…8 mm long.
            CHECK(PathLength(path) >= 2.0);
            CHECK(PathLength(path) <= 8.0);
        }
    }
    CHECK(thick == doctest::Approx(2 * (60 + 30)));   // 1:2
    // The hidden line is 30 mm on the sheet: dashes with 1.5 mm gaps, a dash at each end.
    REQUIRE(dashes >= 2);
    CHECK(thin == doctest::Approx(30 - (static_cast<double>(dashes) - 1) * kDashGap));
    // Hidden lines are drawn first: a seen line lies over them where they meet.
    CHECK(front.paths.front().weight == LineWeight::Thin);
    CHECK(front.paths.back().weight == LineWeight::Thick);

    // «Невидимые линии» off: only what is seen.
    ModelView view = *ReadModelView(d.sheets[0].views[0]);
    view.hiddenLines = false;
    StoreModelView(d.sheets[0].views[0], view);
    placed = LayoutViews(d.sheets[0], images);
    for (const ViewPath& path : placed[0].paths) CHECK(path.weight == LineWeight::Thick);

    // A view whose model was not read: a box to see and pick, no lines.
    CHECK_FALSE(placed[1].computed);
    CHECK(placed[1].paths.empty());
    CHECK(placed[1].box.max.x - placed[1].box.min.x > 10);
}

TEST_CASE("the dashed line: a whole dash at both ends, gaps of 1.5 mm, short lines in one stroke") {
    const Polyline line{{0, 0}, {30, 0}, {30, 20}};   // 50 mm with a corner
    const std::vector<Polyline> dashes = Dashed(line);
    REQUIRE(dashes.size() >= 2);
    CHECK(dashes.front().front() == Vec2{0, 0});
    CHECK(dashes.back().back().x == doctest::Approx(30));
    CHECK(dashes.back().back().y == doctest::Approx(20));
    double sum = 0;
    for (std::size_t i = 0; i < dashes.size(); ++i) {
        const double length = TotalLength({dashes[i]});
        CHECK(length == doctest::Approx(TotalLength({dashes[0]})));   // all the same
        CHECK(length >= 2.0);
        CHECK(length <= 8.0);
        sum += length;
    }
    CHECK(sum == doctest::Approx(50 - (static_cast<double>(dashes.size()) - 1) * kDashGap));
    // A dash that goes round the corner keeps the corner point.
    bool corner = false;
    for (const Polyline& dash : dashes)
        for (std::size_t i = 1; i + 1 < dash.size(); ++i) corner = corner || dash[i] == Vec2{30, 0};
    CHECK(corner);

    CHECK(Dashed({{0, 0}, {5, 0}}).size() == 1);   // too short for two dashes
    CHECK(Dashed({{0, 0}, {0, 0}}).empty());
}

TEST_CASE("overlapping lines: drawn once, and a seen line wins over a hidden one") {
    ViewLines lines;
    lines.visible.push_back({{0, 0}, {100, 0}});
    lines.visible.push_back({{100, 0}, {0, 0}});               // the same edge again, the other way
    lines.visible.push_back({{50, 0}, {150, 0}});              // half of it is new
    lines.visible.push_back({{20, 0}, {20, 0}});               // an edge seen end-on
    lines.hidden.push_back({{-20, 0}, {30, 0}});               // 20 mm stick out to the left
    lines.hidden.push_back({{60, -10}, {60, 10}});             // crosses the seen line: stays
    lines.hidden.push_back({{60, 10}, {60, -10}});             // and a copy of it
    RemoveOverlaps(lines, 0.01);
    CHECK(TotalLength(lines.visible) == doctest::Approx(150).epsilon(1e-3));
    CHECK(TotalLength(lines.hidden) == doctest::Approx(20 + 20).epsilon(2e-3));   // less a hair at the crossing
    CHECK(Covers(lines.hidden, {-20, 0}, {-0.1, 0}, 0.01));
    CHECK_FALSE(Covers(lines.hidden, {1, 0}, {30, 0}, 0.01));
    CHECK(lines.min.x == doctest::Approx(-20));
    CHECK(lines.max.x == doctest::Approx(150));
    CHECK(lines.min.y == doctest::Approx(-10));
    CHECK(lines.max.y == doctest::Approx(10));

    ViewLines nothing;
    RemoveOverlaps(nothing, 0.01);
    CHECK(nothing.Empty());
}

TEST_CASE("standard views: a scale of ГОСТ 2.302 that fits, clear of the title block") {
    Drawing d;
    const SheetLayout a3 = LayoutSheet(d.sheets[0], 1, 1);
    const auto check = [&](const StandardViewsPlan& plan, const ViewLines& front, const ViewLines& top, const ViewLines& left) {
        const double f = plan.scale.Factor();
        const auto box = [&](const ViewLines& l, Vec2 at) { return Box{at + l.min * f, at + l.max * f}; };
        const Box fb = box(front, plan.front), tb = box(top, plan.top), lb = box(left, plan.left);
        // ГОСТ 2.305, first angle: from above — under the main view, from the left — to its right.
        CHECK(tb.max.y < fb.min.y);
        CHECK(lb.min.x > fb.max.x);
        CHECK(plan.top.x == doctest::Approx(plan.front.x));
        CHECK(plan.left.y == doctest::Approx(plan.front.y));
        for (const Box& b : {fb, tb, lb}) {
            CHECK(b.min.x >= a3.frame.min.x);
            CHECK(b.max.x <= a3.frame.max.x);
            CHECK(b.min.y >= a3.frame.min.y);
            CHECK(b.max.y <= a3.frame.max.y);
            const bool overBlock = b.max.x > a3.titleBlock.min.x && b.min.y < a3.titleBlock.max.y;
            CHECK_FALSE(overBlock);
        }
    };
    SUBCASE("a block 120 x 80 x 30 goes on A3 at 1:1") {
        const auto front = BoxLines(120, 30), top = BoxLines(120, 80), left = BoxLines(80, 30);
        const auto plan = PlanStandardViews(a3, *front, *top, *left);
        REQUIRE(plan);
        CHECK(plan->scale == Scale{1, 1});
        check(*plan, *front, *top, *left);
    }
    SUBCASE("a part that fits at 1:1 is not enlarged, though 2:1 would fit as well") {
        const auto front = BoxLines(100, 40), top = BoxLines(100, 40), left = BoxLines(40, 40);
        const auto plan = PlanStandardViews(a3, *front, *top, *left);
        REQUIRE(plan);
        CHECK(plan->scale == Scale{1, 1});
    }
    SUBCASE("a large part is reduced") {
        const auto front = BoxLines(900, 400), top = BoxLines(900, 600), left = BoxLines(600, 400);
        const auto plan = PlanStandardViews(a3, *front, *top, *left);
        REQUIRE(plan);
        CHECK(plan->scale.Factor() < 1);
        check(*plan, *front, *top, *left);
    }
    SUBCASE("a small one is enlarged") {
        const auto front = BoxLines(8, 3), top = BoxLines(8, 6), left = BoxLines(6, 3);
        const auto plan = PlanStandardViews(a3, *front, *top, *left);
        REQUIRE(plan);
        CHECK(plan->scale.Factor() > 1);
        check(*plan, *front, *top, *left);
    }
    SUBCASE("no lines — no plan") {
        const ViewLines empty;
        CHECK_FALSE(PlanStandardViews(a3, empty, empty, empty));
    }
}

TEST_CASE("PDF: views are drawn with the two pens of ГОСТ 2.303") {
    const Expected<TrueTypeFont> font = TrueTypeFont::FromFile(fs::path(BZ_SOURCE_DIR) / "assets" / "fonts" / "IBMPlexSans-Regular.ttf");
    REQUIRE(font);
    const Drawing d = ThreeViews();
    const auto bytes = [&](const ViewImages* images) {
        const std::vector<std::uint8_t> pdf = DrawingPdf(d, *font, "BreezeCAD test", images);
        return Streams(std::string(pdf.begin(), pdf.end()));
    };
    const std::string without = bytes(nullptr);
    const ViewImages images{{ObjectId{10}, BoxLines(120, 60)}};
    const std::string with = bytes(&images);

    // The main view at 1:2 with its origin at (120, 200): the lower edge of the box, thick.
    CHECK(with.find("0.5 w 90 200 m 150 200 l S") != std::string::npos);
    CHECK(without.find("0.5 w 90 200 m 150 200 l S") == std::string::npos);
    // The hidden line in its middle: thin strokes going up from (120, 200), the first a whole dash.
    CHECK(with.find("0.18 w 120 200 m 120 20") != std::string::npos);
    const std::size_t dashes = Count(with, "0.18 w 120 2") - Count(without, "0.18 w 120 2");
    CHECK(dashes >= 4);   // 30 mm of it: about five dashes
    CHECK(Count(with, "0.5 w ") == Count(without, "0.5 w ") + 4);   // the four sides of the box
}
