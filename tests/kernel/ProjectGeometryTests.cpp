// «Проецировать геометрию» до уровня Inventor (06.11.2026): вершины, рабочая
// геометрия и начало координат, кривые других эскизов, сечение, развёртка,
// подрезка перекрывающихся петель контура. Каждый вид следует за моделью при
// перестроении и сохраняется в файл.
#include "../Fixtures.h"
#include "document/SheetMetalFeature.h"
#include "document/WorkPlaneFeature.h"
#include "kernel/Model.h"
#include "kernel/ProjectionSources.h"
#include "kernel/occt/EdgeMatch.h"
#include "kernel/occt/ModelImpl.h"
#include "kernel/occt/Project.h"
#include "app/sketch/ProjectInto.h"
#include "document/EdgeFeature.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <TopExp.hxx>
#include <TopoDS_Compound.hxx>
#include <functional>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <doctest.h>
#include <nlohmann/json.hpp>

// The fast set: every pull request runs it (bz_tests -ts=fast, STAB-0 step 4).
TEST_SUITE_BEGIN("fast");

using namespace bz;
using namespace bz::test;
using doc::FeatureState;
using doctest::Approx;
using Of = doc::Projection::Of;

namespace {

/// The edge of the last body with its middle at `middle`, described.
doc::EdgeReference EdgeAt(const kernel::Model& model, Vec3 middle) {
    const TopoDS_Shape& body = model.GetImpl().bodies.back().shape;
    TopTools_IndexedMapOfShape edges;
    TopExp::MapShapes(body, TopAbs_EDGE, edges);
    for (int i = 1; i <= edges.Extent(); ++i) {
        const BRepAdaptor_Curve c(TopoDS::Edge(edges(i)));
        const gp_Pnt m = c.Value((c.FirstParameter() + c.LastParameter()) / 2);
        if ((Vec3{m.X(), m.Y(), m.Z()} - middle).Length() < 1e-6) return kernel::occt::DescribeEdge(TopoDS::Edge(edges(i)), body);
    }
    FAIL("no edge there");
    return {};
}

void Rebuild(doc::Document& d, kernel::Model& model) {
    d.MarkChanged(0);
    model.Rebuild(d);
}

/// A sketch on a frame of its own (a plane through the part).
doc::SketchFeature& SketchOn(doc::Document& d, const Frame& frame) {
    doc::SketchFeature& sk = AddSketch(d, doc::origin::kPlaneXY);
    sk.placement.frame = frame;
    return sk;
}

ObjectId ProjectedPoint(doc::Document& d, doc::SketchFeature& sk, Vec2 at) {
    const ObjectId p = sk.sketch.AddPoint(d.Ids(), at);
    sk.sketch.FindPoint(p)->projected = true;
    return p;
}

ObjectId ProjectedLine(doc::Document& d, doc::SketchFeature& sk, Vec2 a, Vec2 b) {
    const ObjectId l = sk.sketch.AddLine(d.Ids(), ProjectedPoint(d, sk, a), ProjectedPoint(d, sk, b));
    sk.sketch.FindLine(l)->projected = true;
    return l;
}

/// A 40×20×10 box on XY, its base sketch's corner at (40, 20) movable.
struct Box {
    doc::Document d;
    kernel::Model model;
    doc::SketchFeature* base = nullptr;

    Box() {
        base = &AddSketch(d);
        AddRect(d, *base, {0, 0}, {40, 20});
        AddExtrude(d, base->id, "10");
        model.Rebuild(d);
    }
    /// The box made `width` wide (the rectangle's right side moved).
    void Widen(double width) {
        for (sketch::Point& p : base->sketch.points)
            if (std::abs(p.pos.x - 40.0) < 1e-9) p.pos.x = width;
        Rebuild(d, model);
    }
};

} // namespace

TEST_CASE("проецирование вершины: точка следует за вершиной модели") {
    Box b;
    doc::SketchFeature& sk = AddSketch(b.d);
    doc::Projection p;
    p.of = Of::Vertex;
    p.edge = EdgeAt(b.model, {20, 20, 10});   // the top back edge, ends at (40, 20, 10)
    p.at = {40, 20, 10};
    p.curve = ProjectedPoint(b.d, sk, {40, 20});
    sk.projections.push_back(p);
    Rebuild(b.d, b.model);
    REQUIRE(sk.status.state == FeatureState::Ok);
    CHECK(sk.lostProjections.empty());

    b.Widen(55);
    CHECK(sk.lostProjections.empty());
    const sketch::Point* pt = sk.sketch.FindPoint(p.curve);
    REQUIRE(pt);
    CHECK(pt->pos.x == Approx(55));
    CHECK(pt->pos.y == Approx(20));
    CHECK(sk.projections.front().at.x == Approx(55));
}

TEST_CASE("проецирование рабочей геометрии и начала координат") {
    doc::Document d;
    // A sketch on a plane 5 mm above XY.
    const Frame above{{0, 0, 5}, {1, 0, 0}, {0, 0, 1}};
    SUBCASE("начало координат и оси") {
        const auto origin = kernel::ProjectWork(d, doc::origin::kPoint, above, 30);
        REQUIRE(origin);
        REQUIRE(origin->point);
        CHECK(origin->point->Length() == Approx(0).epsilon(1e-9));
        const auto z = kernel::ProjectWork(d, doc::origin::kAxisZ, above, 30);   // square to the sketch: a point
        REQUIRE(z);
        CHECK(z->point.has_value());
        const auto x = kernel::ProjectWork(d, doc::origin::kAxisX, above, 30);
        REQUIRE(x);
        REQUIRE(x->curve);
        CHECK(std::abs(x->curve->start.y) < 1e-9);
        CHECK((x->curve->end - x->curve->start).Length() == Approx(60));
    }
    SUBCASE("плоскость: линия пересечения; параллельная — ошибка") {
        const auto yz = kernel::ProjectWork(d, doc::origin::kPlaneYZ, above, 25);
        REQUIRE(yz);
        REQUIRE(yz->curve);
        CHECK(std::abs(yz->curve->start.x) < 1e-9);
        CHECK(std::abs(yz->curve->end.x) < 1e-9);
        CHECK((yz->curve->end - yz->curve->start).Length() == Approx(50));
        CHECK_FALSE(kernel::ProjectWork(d, doc::origin::kPlaneXY, above, 25));
    }
    SUBCASE("рабочая плоскость со смещением: линия следует за ней, длина сохраняется") {
        auto wp = std::make_unique<doc::WorkPlaneFeature>();
        wp->id = d.Ids().Next();
        wp->name = "Плоскость1";
        wp->base = doc::origin::kPlaneYZ;
        wp->offset.name = d.NextParameterName();
        wp->offset.expression = "15";
        wp->offset.value = 15;
        doc::WorkPlaneFeature& plane = *wp;
        d.Insert(std::move(wp), 0);
        doc::SketchFeature& sk = AddSketch(d);
        kernel::Model model;
        model.Rebuild(d);
        const auto first = kernel::ProjectWork(d, plane.id, sk.placement.frame, 20);
        REQUIRE(first);
        REQUIRE(first->curve);
        doc::Projection p;
        p.of = Of::Work;
        p.work = plane.id;
        p.curve = ProjectedLine(d, sk, first->curve->start, first->curve->end);
        sk.projections.push_back(p);
        plane.offset.expression = "25";
        plane.offset.value = 25;
        Rebuild(d, model);
        CHECK(sk.lostProjections.empty());
        const sketch::Line* l = sk.sketch.FindLine(p.curve);
        REQUIRE(l);
        CHECK(sk.sketch.PositionOf(l->start).x == Approx(25));
        CHECK(sk.sketch.PositionOf(l->end).x == Approx(25));
        CHECK((sk.sketch.PositionOf(l->end) - sk.sketch.PositionOf(l->start)).Length() == Approx(40));
    }
}

TEST_CASE("проецирование кривой другого эскиза: следует за ней, без ребра тела") {
    doc::Document d;
    doc::SketchFeature& source = AddSketch(d);
    const ObjectId centre = source.sketch.AddPoint(d.Ids(), {10, 10});
    const ObjectId circle = source.sketch.AddCircle(d.Ids(), centre, 5);
    doc::SketchFeature& same = AddSketch(d);                                // the same plane: the circle
    doc::SketchFeature& side = AddSketch(d, doc::origin::kPlaneXZ);         // from the side: a line
    const doc::SketchSource src{source.id, circle, 0.0, 0.0, 1.0};

    const auto flat = kernel::ProjectSketchEntity(d, src, same.placement.frame);
    REQUIRE(flat);
    REQUIRE(flat->curve);
    CHECK(flat->curve->kind == kernel::ProjectedCurve::Kind::Circle);
    const auto edgeOn = kernel::ProjectSketchEntity(d, src, side.placement.frame);
    REQUIRE(edgeOn);
    REQUIRE(edgeOn->curve);
    CHECK(edgeOn->curve->kind == kernel::ProjectedCurve::Kind::Line);
    CHECK((edgeOn->curve->end - edgeOn->curve->start).Length() == Approx(10));

    // Linked: a bigger circle in the source makes a bigger projection.
    const ObjectId c = same.sketch.AddCircle(d.Ids(), ProjectedPoint(d, same, {10, 10}), 5);
    same.sketch.FindCircle(c)->projected = true;
    doc::Projection p;
    p.of = Of::Sketch;
    p.source = src;
    p.curve = c;
    same.projections.push_back(p);
    kernel::Model model;
    model.Rebuild(d);
    source.sketch.FindCircle(circle)->radius = 8;
    Rebuild(d, model);
    CHECK(same.lostProjections.empty());
    CHECK(same.sketch.FindCircle(c)->radius == Approx(8));

    // A sketch below in the tree is not built yet: lost, not followed.
    d.Move(source.id, d.FeatureCount());
    Rebuild(d, model);
    CHECK(same.lostProjections.count(c) == 1);
}

TEST_CASE("окружность под наклоном проецируется эллипсом, дуга — нет") {
    const Frame xy{{0, 0, 0}, {1, 0, 0}, {0, 0, 1}};
    const double s = std::sqrt(0.5);
    const Frame tilted{{0, 0, 0}, {1, 0, 0}, {0, -s, s}};
    kernel::PlanarCurve circle;
    circle.kind = kernel::PlanarCurve::Kind::Circle;
    circle.radius = 10;
    const auto e = kernel::ProjectPlanar(xy, circle, tilted);
    REQUIRE(e);
    REQUIRE(e->curve);
    CHECK(e->curve->kind == kernel::ProjectedCurve::Kind::Ellipse);
    CHECK((e->curve->start - e->curve->centre).Length() == Approx(10));
    CHECK((e->curve->end - e->curve->centre).Length() == Approx(10 * s));
    kernel::PlanarCurve arc = circle;
    arc.kind = kernel::PlanarCurve::Kind::Arc;
    arc.a = {10, 0};
    arc.b = {0, 10};
    CHECK_FALSE(kernel::ProjectPlanar(xy, arc, tilted));
    // From behind, an arc runs the other way round.
    const Frame back{{0, 0, 0}, {1, 0, 0}, {0, 0, -1}};
    const auto r = kernel::ProjectPlanar(xy, arc, back);
    REQUIRE(r);
    REQUIRE(r->curve);
    CHECK(r->curve->kind == kernel::ProjectedCurve::Kind::Arc);
}

TEST_CASE("проецирование сечения: всё, что режет плоскость эскиза") {
    Box b;
    // A plane square to Y through the middle of the box: a 40×10 rectangle.
    const Frame middle{{0, 10, 0}, {1, 0, 0}, {0, -1, 0}};
    doc::SketchFeature& sk = SketchOn(b.d, middle);
    Rebuild(b.d, b.model);
    const auto cut = b.model.ProjectSection(sk.placement.frame, static_cast<std::size_t>(b.d.IndexOf(sk.id)));
    REQUIRE(cut.size() == 4);
    double length = 0;
    for (const kernel::ProjectedCurve& c : cut) {
        CHECK(c.kind == kernel::ProjectedCurve::Kind::Line);
        length += (c.end - c.start).Length();
    }
    CHECK(length == Approx(100));
    // A plane beside the box cuts nothing.
    const Frame beside{{0, 30, 0}, {1, 0, 0}, {0, -1, 0}};
    CHECK(b.model.ProjectSection(beside, static_cast<std::size_t>(b.d.IndexOf(sk.id))).empty());
}

TEST_CASE("контур грани: перекрытия подрезаются, лишнее не дублируется") {
    using K = kernel::ProjectedCurve::Kind;
    const auto line = [](Vec2 a, Vec2 b) {
        kernel::ProjectedCurve c;
        c.start = a;
        c.end = b;
        return c;
    };
    bool changed = true;
    SUBCASE("ничего не пересекается — как было") {
        const auto out = kernel::TrimOverlappingCurves({line({0, 0}, {10, 0}), line({0, 5}, {10, 5})}, changed);
        CHECK_FALSE(changed);
        CHECK(out.size() == 2);
    }
    SUBCASE("отрезки на одной прямой внахлёст — один отрезок") {
        const auto out = kernel::TrimOverlappingCurves({line({0, 0}, {10, 0}), line({5, 0}, {15, 0})}, changed);
        CHECK(changed);
        REQUIRE(out.size() == 1);
        CHECK((out[0].end - out[0].start).Length() == Approx(15));
    }
    SUBCASE("окружность через отрезок — разрезаны в точках пересечения") {
        kernel::ProjectedCurve circle;
        circle.kind = K::Circle;
        circle.radius = 5;
        const auto out = kernel::TrimOverlappingCurves({circle, line({-10, 0}, {10, 0})}, changed);
        CHECK(changed);
        CHECK(std::ranges::count_if(out, [](const auto& c) { return c.kind == K::Arc; }) == 2);
        CHECK(std::ranges::count_if(out, [](const auto& c) { return c.kind == K::Line; }) == 3);
    }
}

TEST_CASE("проецирование развёртки листовой детали следует за ней") {
    doc::Document d;
    auto& base = AddSketch(d);
    AddRect(d, base, {0, 0}, {100, 50});
    auto sheet = std::make_unique<doc::SheetMetalFeature>();
    sheet->id = d.Ids().Next();
    sheet->name = "Лист";
    sheet->sketch = base.id;
    sheet->thickness.name = "s";
    sheet->thickness.expression = "2";
    const ObjectId sheetId = sheet->id;
    d.Insert(std::move(sheet), d.FeatureCount());
    auto flange = std::make_unique<doc::FlangeFeature>();
    flange->id = d.Ids().Next();
    flange->name = "Отбортовка";
    flange->sheet = sheetId;
    flange->edge = {50, 0, 0};
    flange->edgeSet = true;
    flange->height.expression = "20";
    flange->angle.expression = "90";
    doc::FlangeFeature& fl = *flange;
    d.Insert(std::move(flange), d.FeatureCount());
    doc::SketchFeature& sk = AddSketch(d);
    kernel::Model model;
    model.Rebuild(d);
    const std::size_t at = static_cast<std::size_t>(d.IndexOf(sk.id));
    const auto info = model.SheetBefore(d, sheetId, at);
    REQUIRE(info);
    const std::vector<kernel::PlanarCurve> curves = kernel::FlatPatternCurves(*info);
    CHECK(kernel::FlatPatternBends(*info) == 1);
    REQUIRE(curves.size() > 4);
    double low = 0;   // how far the blank reaches down (−Y): the flange unrolled
    for (std::size_t i = 0; i < curves.size(); ++i) {
        const auto item = kernel::ProjectPlanar(info->frame, curves[i], sk.placement.frame);
        REQUIRE(item);
        REQUIRE(item->curve);
        REQUIRE(item->curve->kind == kernel::ProjectedCurve::Kind::Line);
        doc::Projection p;
        p.of = Of::Flat;
        p.work = sheetId;
        p.part = static_cast<int>(i);
        p.curve = ProjectedLine(d, sk, item->curve->start, item->curve->end);
        sk.projections.push_back(p);
        low = std::min({low, item->curve->start.y, item->curve->end.y});
    }
    CHECK(low < -15);
    fl.height.expression = "30";
    Rebuild(d, model);
    CHECK(sk.lostProjections.empty());
    double lowAfter = 0;
    for (const sketch::Point& p : sk.sketch.points) lowAfter = std::min(lowAfter, p.pos.y);
    CHECK(lowAfter < low - 5);
}

TEST_CASE("новые виды проекций сохраняются и читаются") {
    doc::SketchFeature sk;
    sk.id = ObjectId{100};
    sk.placement = {doc::origin::kPlaneXY, *doc::origin::PlaneFrame(doc::origin::kPlaneXY), false, {}};
    doc::Projection vertex;
    vertex.of = Of::Vertex;
    vertex.edge.middle = {1, 2, 3};
    vertex.at = {4, 5, 6};
    vertex.curve = ObjectId{101};
    doc::Projection work;
    work.of = Of::Work;
    work.work = doc::origin::kAxisX;
    work.curve = ObjectId{102};
    doc::Projection other;
    other.of = Of::Sketch;
    other.source = {ObjectId{50}, ObjectId{51}, 0.0, 0.0, 1.0};
    other.curve = ObjectId{103};
    doc::Projection flat;
    flat.of = Of::Flat;
    flat.work = ObjectId{60};
    flat.part = 7;
    flat.curve = ObjectId{104};
    sk.projections = {vertex, work, other, flat};
    nlohmann::json j;
    sk.SaveFields(j);
    CHECK_FALSE(j["projections"][1].contains("edge"));   // only edges and vertices keep an edge
    doc::SketchFeature back;
    REQUIRE(back.LoadFields(j).Ok());
    REQUIRE(back.projections.size() == 4);
    CHECK(back.projections[0].of == Of::Vertex);
    CHECK(back.projections[0].at.y == Approx(5));
    CHECK(back.projections[0].edge.middle.z == Approx(3));
    CHECK(back.projections[1].of == Of::Work);
    CHECK(back.projections[1].work == doc::origin::kAxisX);
    CHECK(back.projections[2].of == Of::Sketch);
    CHECK(back.projections[2].source.curve == ObjectId{51});
    CHECK(back.projections[3].of == Of::Flat);
    CHECK(back.projections[3].part == 7);
    CHECK(back.projections[3].work == ObjectId{60});
    // A file of an older version (an edge without "of") reads as an edge.
    nlohmann::json old = j;
    old["projections"] = nlohmann::json::array({old["projections"][0]});
    old["projections"][0].erase("of");
    old["projections"][0].erase("at");
    doc::SketchFeature older;
    REQUIRE(older.LoadFields(old).Ok());
    CHECK(older.projections.front().of == Of::Edge);
}

// Деталь из двух тел: проекция ребра первого тела искалась только на
// последнем теле — терялась при первом же пересчёте или молча прыгала на
// похожее ребро второго тела (стабилизация, 13.11.2026).
TEST_CASE("проекция ребра не последнего тела детали следует за ним") {
    doc::Document d;
    kernel::Model model;
    doc::SketchFeature& first = AddSketch(d);
    AddRect(d, first, {0, 0}, {40, 20});
    AddExtrude(d, first.id, "10");
    doc::SketchFeature& second = AddSketch(d);
    AddRect(d, second, {100, 0}, {140, 20});
    AddExtrude(d, second.id, "10", doc::BooleanOp::NewBody);
    model.Rebuild(d);
    REQUIRE(model.GetImpl().bodies.size() == 2);

    // The top front edge of the FIRST body (middle at (20, 0, 10)).
    doc::EdgeReference edge;
    {
        const TopoDS_Shape& body = model.GetImpl().bodies.front().shape;
        TopTools_IndexedMapOfShape edges;
        TopExp::MapShapes(body, TopAbs_EDGE, edges);
        for (int i = 1; i <= edges.Extent(); ++i) {
            const BRepAdaptor_Curve c(TopoDS::Edge(edges(i)));
            const gp_Pnt m = c.Value((c.FirstParameter() + c.LastParameter()) / 2);
            if ((Vec3{m.X(), m.Y(), m.Z()} - Vec3{20, 0, 10}).Length() < 1e-6) edge = kernel::occt::DescribeEdge(TopoDS::Edge(edges(i)), body);
        }
    }
    doc::SketchFeature& sk = AddSketch(d);
    const auto at = static_cast<std::size_t>(d.IndexOf(sk.id));
    Rebuild(d, model);
    const auto projected = model.ProjectEdge(edge, sk.placement.frame, at);
    REQUIRE(projected);
    CHECK(projected->start.y == Approx(0));
    CHECK(std::abs(projected->end.x - projected->start.x) == Approx(40));
    doc::Projection p;
    p.edge = edge;
    p.curve = ProjectedLine(d, sk, projected->start, projected->end);
    sk.projections.push_back(p);
    Rebuild(d, model);
    CHECK(sk.lostProjections.empty());

    // The first body made 55 wide: the projected line follows it, not the second body.
    for (sketch::Point& pt : first.sketch.points)
        if (std::abs(pt.pos.x - 40.0) < 1e-9) pt.pos.x = 55;
    Rebuild(d, model);
    CHECK(sk.lostProjections.empty());
    const sketch::Line* l = sk.sketch.FindLine(p.curve);
    REQUIRE(l);
    const double x0 = sk.sketch.PositionOf(l->start).x, x1 = sk.sketch.PositionOf(l->end).x;
    CHECK(std::min(x0, x1) == Approx(0));
    CHECK(std::max(x0, x1) == Approx(55));
}

// Ребро-сплайн (выдавленный сплайн) и наклонная дуга проецировались с
// ошибкой «Такое ребро пока не проецируется». Теперь — сплайном через точки,
// который следует за моделью (как в Inventor).
TEST_CASE("ребро-сплайн проецируется сплайном и следует за моделью") {
    doc::Document d;
    kernel::Model model;
    doc::SketchFeature& base = AddSketch(d);
    IdGenerator& ids = d.Ids();
    sketch::Sketch& s = base.sketch;
    const ObjectId a = s.AddPoint(ids, {0, 0}), m = s.AddPoint(ids, {20, 8}), b = s.AddPoint(ids, {40, 0});
    const ObjectId c = s.AddPoint(ids, {40, -10}), e = s.AddPoint(ids, {0, -10});
    s.AddSpline(ids, {a, m, b}, true);
    s.AddLine(ids, b, c);
    s.AddLine(ids, c, e);
    s.AddLine(ids, e, a);
    AddExtrude(d, base.id, "10");
    model.Rebuild(d);
    REQUIRE(base.status.state == FeatureState::Ok);

    // The top edge along the spline: its middle near (20, 8, 10).
    doc::EdgeReference edge;
    double best = 1e9;
    {
        const TopoDS_Shape& body = model.GetImpl().bodies.back().shape;
        TopTools_IndexedMapOfShape edges;
        TopExp::MapShapes(body, TopAbs_EDGE, edges);
        for (int i = 1; i <= edges.Extent(); ++i) {
            const BRepAdaptor_Curve cv(TopoDS::Edge(edges(i)));
            const gp_Pnt mid = cv.Value((cv.FirstParameter() + cv.LastParameter()) / 2);
            const double miss = (Vec3{mid.X(), mid.Y(), mid.Z()} - Vec3{20, 8, 10}).Length();
            if (miss < best) {
                best = miss;
                edge = kernel::occt::DescribeEdge(TopoDS::Edge(edges(i)), body);
            }
        }
    }
    REQUIRE(best < 0.5);
    doc::SketchFeature& sk = AddSketch(d);
    const auto at = static_cast<std::size_t>(d.IndexOf(sk.id));
    Rebuild(d, model);
    const auto projected = model.ProjectEdge(edge, sk.placement.frame, at);
    REQUIRE(projected);
    REQUIRE(projected->kind == kernel::ProjectedCurve::Kind::Spline);
    REQUIRE(projected->points.size() == static_cast<std::size_t>(kernel::kSplineSpans + 1));
    const double x0 = projected->points.front().x, x1 = projected->points.back().x;
    CHECK(std::min(x0, x1) == Approx(0).epsilon(1e-6));
    CHECK(std::max(x0, x1) == Approx(40).epsilon(1e-6));
    double top = -1e9;
    for (const Vec2& pt : projected->points) top = std::max(top, pt.y);
    CHECK(top == Approx(8).epsilon(0.02));

    const ObjectId curve = kernel::AddProjectedSpline(sk.sketch, ids, *projected, [&](Vec2 p) {
        const ObjectId id = sk.sketch.AddPoint(ids, p);
        sk.sketch.FindPoint(id)->projected = true;
        return id;
    });
    REQUIRE(curve.IsValid());
    doc::Projection p;
    p.edge = edge;
    p.curve = curve;
    sk.projections.push_back(p);
    Rebuild(d, model);
    CHECK(sk.lostProjections.empty());

    // The bump made higher: the projected spline follows.
    s.FindPoint(m)->pos.y = 14;
    Rebuild(d, model);
    CHECK(sk.lostProjections.empty());
    const sketch::Spline* sp = sk.sketch.FindSpline(curve);
    REQUIRE(sp);
    double high = -1e9;
    for (ObjectId id : sp->points) high = std::max(high, sk.sketch.PositionOf(id).y);
    CHECK(high == Approx(14).epsilon(0.02));
}

TEST_CASE("дуга под наклоном к эскизу проецируется сплайном, а не ошибкой") {
    doc::Document d;
    kernel::Model model;
    doc::SketchFeature& base = AddSketch(d);
    AddCircle(d, base, {0, 0}, 10);
    AddExtrude(d, base.id, "20");
    model.Rebuild(d);
    // Half the top rim seen on a plane tilted 45° about X: a piece of an ellipse.
    const TopoDS_Shape& body = model.GetImpl().bodies.back().shape;
    std::optional<TopoDS_Edge> rim;
    TopTools_IndexedMapOfShape edges;
    TopExp::MapShapes(body, TopAbs_EDGE, edges);
    for (int i = 1; i <= edges.Extent() && !rim; ++i) {
        const BRepAdaptor_Curve cv(TopoDS::Edge(edges(i)));
        if (cv.GetType() == GeomAbs_Circle && std::abs(cv.Value(cv.FirstParameter()).Z() - 20) < 1e-6) rim = TopoDS::Edge(edges(i));
    }
    REQUIRE(rim);
    BRepAdaptor_Curve cv(*rim);
    const double t0 = cv.FirstParameter();
    const TopoDS_Edge half = BRepBuilderAPI_MakeEdge(cv.Curve().Curve(), t0, t0 + 3.14159265358979).Edge();
    const double s = std::sqrt(0.5);
    const Frame tilted{{0, 0, 0}, {1, 0, 0}, {0, -s, s}};
    const auto pc = kernel::occt::ProjectEdge(half, tilted);
    REQUIRE(pc);
    CHECK(pc->kind == kernel::ProjectedCurve::Kind::Spline);
    for (const Vec2& p : pc->points) {
        // On the ellipse: x²/10² + (y/(10·s))²·… — every point within the rim's shadow.
        CHECK(std::abs(p.x) <= 10 + 1e-9);
    }
}

// --- Испытание: проецируем ВСЕ рёбра детали на несколько плоскостей, меняем
// размер — каждая связанная проекция должна остаться на модели (ни одна не
// потеряна, ни одна не прыгнула на чужое ребро), эскиз решается.

namespace {

std::vector<std::vector<Vec2>> ShadowOfEdges(const TopoDS_Shape& shape, const Frame& frame) {
    std::vector<std::vector<Vec2>> out;
    TopTools_IndexedMapOfShape edges;
    TopExp::MapShapes(shape, TopAbs_EDGE, edges);
    for (int i = 1; i <= edges.Extent(); ++i) {
        const BRepAdaptor_Curve c(TopoDS::Edge(edges(i)));
        std::vector<Vec2> poly;
        for (int k = 0; k <= 400; ++k) {
            const gp_Pnt p = c.Value(c.FirstParameter() + (c.LastParameter() - c.FirstParameter()) * k / 400);
            poly.push_back(frame.ToLocal({p.X(), p.Y(), p.Z()}));
        }
        out.push_back(std::move(poly));
    }
    return out;
}

double MissFromShadow(const std::vector<std::vector<Vec2>>& shadow, Vec2 p) {
    double best = 1e300;
    for (const auto& poly : shadow)
        for (std::size_t i = 0; i + 1 < poly.size(); ++i) {
            const Vec2 a = poly[i], b = poly[i + 1], d = b - a;
            const double len2 = d.Dot(d);
            const double t = len2 > 0 ? std::clamp((p - a).Dot(d) / len2, 0.0, 1.0) : 0.0;
            best = std::min(best, (a + d * t - p).Length());
        }
    return best;
}

TopoDS_Shape SolidsOf(const kernel::Model& model) {
    TopoDS_Compound all;
    BRep_Builder builder;
    builder.MakeCompound(all);
    for (const auto& b : model.GetImpl().bodies)
        if (!b.mesh) builder.Add(all, b.shape);
    return all;
}

struct TortureResult {
    int projected = 0, refused = 0, lost = 0, off = 0, broken = 0;
};

/// Builds by `make`, projects every edge onto `frames`, applies `change`, rebuilds and checks.
template <class Make, class Change>
TortureResult Torture(Make make, Change change, const std::vector<Frame>& frames) {
    TortureResult r;
    doc::Document d;
    kernel::Model model;
    make(d);
    model.Rebuild(d);
    const TopoDS_Shape before = SolidsOf(model);
    std::vector<doc::SketchFeature*> sketches;
    for (const Frame& f : frames) sketches.push_back(&SketchOn(d, f));
    Rebuild(d, model);
    for (doc::SketchFeature* sk : sketches) {
        const auto index = static_cast<std::size_t>(d.IndexOf(sk->id));
        TopTools_IndexedMapOfShape edges;
        TopExp::MapShapes(before, TopAbs_EDGE, edges);
        app::Projector put(d, *sk, 1e-6, true, false);
        for (int i = 1; i <= edges.Extent(); ++i) {
            const TopoDS_Edge& e = TopoDS::Edge(edges(i));
            if (BRep_Tool::Degenerated(e)) continue;
            const doc::EdgeReference ref = kernel::occt::DescribeEdge(e, before);
            const auto pc = model.ProjectEdge(ref, sk->placement.frame, index);
            if (!pc) {
                ++r.refused;
                continue;
            }
            doc::Projection link;
            link.edge = ref;
            if (put.Curve(*pc, &link)) ++r.projected;
        }
    }
    Rebuild(d, model);
    for (doc::SketchFeature* sk : sketches) CHECK(sk->lostProjections.empty());
    change(d);
    Rebuild(d, model);
    const TopoDS_Shape after = SolidsOf(model);
    for (doc::SketchFeature* sk : sketches) {
        r.lost += static_cast<int>(sk->lostProjections.size());
        if (sk->status.state != FeatureState::Ok) ++r.broken;
        const auto shadow = ShadowOfEdges(after, sk->placement.frame);
        for (const doc::Projection& p : sk->projections) {
            if (sk->lostProjections.count(p.curve)) continue;
            std::vector<Vec2> pts;
            if (const sketch::Line* l = sk->sketch.FindLine(p.curve)) pts = {sk->sketch.PositionOf(l->start), sk->sketch.PositionOf(l->end)};
            else if (const sketch::Arc* a = sk->sketch.FindArc(p.curve)) pts = {sk->sketch.PositionOf(a->start), sk->sketch.PositionOf(a->end)};
            else if (const sketch::Circle* c = sk->sketch.FindCircle(p.curve)) pts = {sk->sketch.PositionOf(c->center) + Vec2{c->radius, 0}};
            else if (const sketch::Spline* s = sk->sketch.FindSpline(p.curve))
                for (ObjectId id : s->points) pts.push_back(sk->sketch.PositionOf(id));
            else if (const sketch::Ellipse* e = sk->sketch.FindEllipse(p.curve)) pts = {sk->sketch.PositionOf(e->major)};
            for (const Vec2& q : pts)
                if (MissFromShadow(shadow, q) > 1e-3) {
                    ++r.off;
                    break;
                }
        }
    }
    return r;
}

std::vector<Frame> TortureFrames() {
    const double s = std::sqrt(0.5);
    return {Frame{{0, 0, 30}, {1, 0, 0}, {0, 0, 1}},     // above
            Frame{{0, -60, 0}, {1, 0, 0}, {0, -1, 0}},   // in front
            Frame{{80, 0, 0}, {0, 1, 0}, {1, 0, 0}},     // right
            Frame{{0, 0, 40}, {1, 0, 0}, {0, -s, s}}};   // slanted
}

void WidenBase(doc::Document& d, double from, double to) {
    auto* base = dynamic_cast<doc::SketchFeature*>(d.Features().front().get());
    for (sketch::Point& p : base->sketch.points)
        if (std::abs(p.pos.x - from) < 1e-9) p.pos.x = to;
}

} // namespace

TEST_CASE("испытание проекций: все рёбра детали следуют за изменением размера") {
    struct Case {
        const char* name;
        std::function<void(doc::Document&)> make;
    };
    const std::vector<Case> cases = {
        {"коробка", [](doc::Document& d) {
             doc::SketchFeature& b = AddSketch(d);
             AddRect(d, b, {0, 0}, {40, 20});
             AddExtrude(d, b.id, "10");
         }},
        {"пластина с отверстием", [](doc::Document& d) {
             doc::SketchFeature& b = AddSketch(d);
             AddRect(d, b, {0, 0}, {40, 20});
             AddExtrude(d, b.id, "10");
             doc::SketchFeature& h = AddSketch(d);
             AddCircle(d, h, {10, 10}, 4);
             AddExtrude(d, h.id, "10", doc::BooleanOp::Cut);
         }},
        {"коробка со скруглением", [](doc::Document& d) {
             doc::SketchFeature& b = AddSketch(d);
             AddRect(d, b, {0, 0}, {40, 20});
             AddExtrude(d, b.id, "10");
             auto f = std::make_unique<doc::FilletFeature>();
             f->id = d.Ids().Next();
             f->name = "Скругление";
             doc::EdgeReference e;
             e.curve = doc::CurveKind::Line;
             e.direction = {0, 0, 1};
             e.middle = {0, 0, 5};
             e.faces = {{{doc::SurfaceKind::Plane, {-1, 0, 0}}, {doc::SurfaceKind::Plane, {0, -1, 0}}}};
             f->edges = {e};
             std::vector<Parameter*> ps;
             f->CollectParameters(ps);
             for (Parameter* p : ps) p->expression = "3";
             d.Insert(std::move(f), d.FeatureCount());
         }},
        {"два тела", [](doc::Document& d) {
             doc::SketchFeature& b = AddSketch(d);
             AddRect(d, b, {0, 0}, {40, 20});
             AddExtrude(d, b.id, "10");
             doc::SketchFeature& c = AddSketch(d);
             AddCircle(d, c, {60, 10}, 6);
             AddExtrude(d, c.id, "15", doc::BooleanOp::NewBody);
         }},
    };
    for (const Case& c : cases) {
        CAPTURE(c.name);
        const TortureResult r = Torture(c.make, [](doc::Document& d) { WidenBase(d, 40, 47); }, TortureFrames());
        MESSAGE(c.name << ": спроецировано " << r.projected << ", отказано " << r.refused << ", потеряно " << r.lost << ", мимо модели "
                       << r.off << ", эскизов с ошибкой " << r.broken);
        CHECK(r.projected > 0);
        CHECK(r.lost == 0);
        CHECK(r.off == 0);
        CHECK(r.broken == 0);
    }
}
TEST_SUITE_END();
