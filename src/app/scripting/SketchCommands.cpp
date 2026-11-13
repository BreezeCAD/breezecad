// Sketches: create one on a plane, add geometry (with its dimensions, so the
// result is parametric at once), add constraints, read the solved result.
// Every call is one undo step.

#include "app/AppContext.h"
#include "app/Session.h"
#include "app/scripting/Commands.h"
#include "app/viewport/Planes.h"
#include "core/I18n.h"
#include "document/Commands.h"
#include "document/SketchFeature.h"
#include "kernel/Projection.h"
#include "sketch/Fillet.h"
#include "sketch/Mirror.h"
#include "sketch/Move.h"
#include "sketch/Offset.h"
#include "sketch/Shapes.h"
#include "sketch/Trim.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace bz::app::scripting {

namespace {

using sketch::Constraint;
using sketch::ConstraintType;

doc::SketchFeature& SketchOf(Session& s, const Json& args) {
    auto* f = dynamic_cast<doc::SketchFeature*>(&FeatureOf(s, Id(args, "sketch")));
    if (!f) Fail("Элемент «sketch» — не эскиз");
    return *f;
}

/// What sketch.add and sketch.constrain build on: a copy of the sketch
/// feature that becomes one undo step when everything went well.
class SketchEdit {
public:
    SketchEdit(Session& s, const doc::SketchFeature& f)
        : mySession(s), myFeature(static_cast<doc::SketchFeature*>(f.Clone().release())), myEdited(myFeature) {
        // Dimension names continue after the largest "dN" of the document.
        const std::string next = s.doc.NextParameterName();
        myNextName = std::stoi(next.substr(1));
    }

    sketch::Sketch&     Sk() { return myFeature->sketch; }
    doc::SketchFeature& Feature() { return *myFeature; }
    Session&            Part() { return mySession; }
    IdGenerator&    Ids() { return mySession.doc.Ids(); }

    /// An existing point at `pos` (within 1 µm), else a new one: shapes drawn
    /// end to end share their corners and close into profiles.
    ObjectId PointAt(Vec2 pos) {
        for (const sketch::Point& p : Sk().points)
            if (!p.projected && (p.pos - pos).Length() < 1e-3) return p.id;
        return Sk().AddPoint(Ids(), pos);
    }

    /// A driving dimension with the next free name ("d7") at the current size.
    ObjectId Dimension(ConstraintType type, ObjectId a, ObjectId b, double value, Vec2 labelOffset,
                       Quantity quantity = Quantity::Length) {
        Constraint c;
        c.type                 = type;
        c.refs                 = {a, b, {}};
        c.parameter.name       = "d" + std::to_string(myNextName++);
        c.parameter.quantity   = quantity;
        c.parameter.value      = value;
        c.parameter.expression = std::format("{}", std::round(mySession.doc.units.ToDisplay(value, quantity) * 1e6) / 1e6);
        c.labelOffset          = labelOffset;
        return Sk().AddConstraint(Ids(), c);
    }

    std::string NameOf(ObjectId constraint) const {
        const Constraint* c = myFeature->sketch.FindConstraint(constraint);
        return c ? c->parameter.name : "";
    }

    Json Commit(std::string label) {
        const ObjectId id = myFeature->id;
        mySession.Execute(doc::ReplaceFeature(std::move(myEdited), std::move(label)));
        return AfterChange(mySession, id);
    }

private:
    Session&                      mySession;
    doc::SketchFeature*           myFeature;
    std::unique_ptr<doc::Feature> myEdited;
    int                           myNextName = 1;
};

Vec2 Outward(Vec2 a, Vec2 b) {   // a dimension label beside the segment a-b
    const Vec2 d = b - a;
    return Vec2{-d.y, d.x}.Normalized() * 8.0;
}

/// Adds one item of sketch.add; returns what was made (ids, dimension names).
Json AddItem(SketchEdit& e, const Json& item) {
    sketch::Sketch&   sk   = e.Sk();
    const std::string type = Text(item, "type");
    const bool        dims = Flag(item, "dimensions", true);
    const bool        aux  = Flag(item, "construction", false);
    Json              out  = {{"type", type}};
    Json              names = Json::array();
    const auto        dim  = [&](ObjectId c) { names.push_back(e.NameOf(c)); };

    if (type == "line" || type == "centerline" || type == "polyline") {
        std::vector<Vec2> pts;
        if (type == "polyline") {
            const Json& list = Required(item, "points");
            if (!list.is_array() || list.size() < 2) Fail("polyline: «points» — минимум две точки");
            for (const Json& p : list) pts.push_back(Point(p, "points"));
        } else {
            pts = {Point(item, "from", std::nullopt), Point(item, "to", std::nullopt)};
        }
        const bool closed = type == "polyline" && Flag(item, "closed", false);
        std::vector<ObjectId> ids;
        for (Vec2 p : pts) ids.push_back(e.PointAt(p));
        if (closed) ids.push_back(ids.front());
        Json lines = Json::array();
        for (std::size_t i = 0; i + 1 < ids.size(); ++i) {
            if (ids[i] == ids[i + 1]) Fail("Отрезок нулевой длины");
            // An осевая is not construction: it bounds profiles (a shaft drawn
            // as a half closes on its axis), as in the program's own tool.
            const ObjectId l = sk.AddLine(e.Ids(), ids[i], ids[i + 1], aux);
            if (type == "centerline") sk.FindLine(l)->centerline = true;
            const Vec2 a = sk.PositionOf(ids[i]), b = sk.PositionOf(ids[i + 1]);
            if (dims && type != "centerline") dim(e.Dimension(ConstraintType::Length, l, {}, (b - a).Length(), Outward(a, b)));
            lines.push_back(l.value);
        }
        out["lines"] = std::move(lines);
    } else if (type == "circle") {
        const ObjectId c = e.PointAt(Point(item, "center", std::nullopt));
        const double   r = Has(item, "diameter") ? Number(item, "diameter") / 2 : Number(item, "radius");
        if (r <= 0) Fail("circle: размер должен быть больше нуля");
        const ObjectId circle = sk.AddCircle(e.Ids(), c, r, aux);
        if (dims) dim(e.Dimension(ConstraintType::Diameter, circle, {}, 2 * r, Vec2{1, 1}.Normalized() * (r + 8)));
        out["circle"] = circle.value;
        out["center"] = c.value;
    } else if (type == "arc") {
        const Vec2 cp = Point(item, "center", std::nullopt), sp = Point(item, "start", std::nullopt);
        Vec2       ep = Point(item, "end", std::nullopt);
        const double r = (sp - cp).Length();
        if (r <= 0) Fail("arc: начало совпадает с центром");
        ep = cp + (ep - cp).Normalized() * r;   // the end on the same radius
        const ObjectId arc = sk.AddArc(e.Ids(), e.PointAt(cp), e.PointAt(sp), e.PointAt(ep), aux);
        if (dims) dim(e.Dimension(ConstraintType::Radius, arc, {}, r, Vec2{1, 1}.Normalized() * (r + 8)));
        out["arc"] = arc.value;
    } else if (type == "rectangle") {
        sketch::RectangleIds r;
        if (Has(item, "center")) {
            const Vec2 c = Point(item, "center", std::nullopt);
            const double w = Number(item, "width"), h = Number(item, "height");
            if (w <= 0 || h <= 0) Fail("rectangle: ширина и высота должны быть больше нуля");
            r = sketch::AddCentredRectangle(sk, e.Ids(), e.PointAt(c), c + Vec2{w / 2, h / 2});
        } else {
            const Vec2 a = Point(item, "corner", std::nullopt), b = Point(item, "opposite", std::nullopt);
            if (std::abs(a.x - b.x) < 1e-6 || std::abs(a.y - b.y) < 1e-6) Fail("rectangle: углы на одной линии");
            r = sketch::AddCornerRectangle(sk, e.Ids(), e.PointAt(a), e.PointAt(b));
        }
        for (int i = 0; i < 2 && dims; ++i) {
            const sketch::Line* l = sk.FindLine(r.sides[static_cast<std::size_t>(i)]);
            const Vec2 a = sk.PositionOf(l->start), b = sk.PositionOf(l->end);
            dim(e.Dimension(ConstraintType::Length, l->id, {}, (b - a).Length(), Outward(a, b)));
        }
        out["sides"] = Json::array();
        for (ObjectId s : r.sides) out["sides"].push_back(s.value);
        out["corners"] = Json::array();
        for (ObjectId c : r.corners) out["corners"].push_back(c.value);
    } else if (type == "slot") {
        const Vec2   a = Point(item, "from", std::nullopt), b = Point(item, "to", std::nullopt);
        const double w = Number(item, "width");
        if (w <= 0 || (b - a).Length() < 1e-6) Fail("slot: нужны два разных центра и ширина больше нуля");
        const sketch::SlotIds slot = sketch::AddStraightSlot(sk, e.Ids(), e.PointAt(a), e.PointAt(b), w);
        if (dims) {
            dim(e.Dimension(ConstraintType::Length, slot.axis, {}, (b - a).Length(), Outward(a, b)));
            dim(e.Dimension(ConstraintType::Diameter, slot.cap, {}, w, Vec2{1, 1}.Normalized() * (w / 2 + 8)));
        }
        out["curves"] = Json::array();
        for (ObjectId c : slot.curves) out["curves"].push_back(c.value);
    } else if (type == "polygon") {
        const Vec2   c      = Point(item, "center", std::nullopt);
        const int    sides  = Integer(item, "sides");
        const double radius = Number(item, "radius");
        const bool   inside = Flag(item, "inscribed", true);
        if (sides < 3 || sides > 64 || radius <= 0) Fail("polygon: от 3 до 64 сторон, радиус больше нуля");
        const sketch::PolygonIds p = sketch::AddPolygon(sk, e.Ids(), e.PointAt(c), c + Vec2{radius, 0}, sides, inside);
        if (dims) dim(e.Dimension(ConstraintType::Diameter, p.circle, {}, 2 * radius, Vec2{1, 1}.Normalized() * (radius + 8)));
        out["sides"] = Json::array();
        for (ObjectId s : p.sides) out["sides"].push_back(s.value);
    } else if (type == "point") {
        const ObjectId p = sk.AddPoint(e.Ids(), Point(item, "at", std::nullopt));
        out["point"] = p.value;
    } else if (type == "spline") {
        const Json& list = Required(item, "points");
        if (!list.is_array() || list.size() < 2) Fail("spline: «points» — минимум две точки");
        std::vector<ObjectId> ids;
        for (const Json& p : list) ids.push_back(e.PointAt(Point(p, "points")));
        if (Flag(item, "closed", false)) {
            if (ids.size() < 3) Fail("spline: замкнутому сплайну нужны минимум три точки");
            ids.push_back(ids.front());
        }
        const ObjectId spline = sk.AddSpline(e.Ids(), ids, Flag(item, "interpolate", true), aux);
        out["spline"] = spline.value;
        out["points"] = Json::array();
        for (ObjectId p : ids) out["points"].push_back(p.value);
    } else if (type == "ellipse") {
        const Vec2   c     = Point(item, "center", std::nullopt);
        const Vec2   major = Point(item, "major", std::nullopt);
        const double minor = Number(item, "minor_radius");
        const Vec2   axis  = major - c;
        if (axis.Length() < 1e-6 || minor <= 0) Fail("ellipse: конец большой оси не в центре, minor_radius больше нуля");
        const Vec2 side = Vec2{-axis.y, axis.x}.Normalized() * minor;
        const ObjectId ellipse = sk.AddEllipse(e.Ids(), e.PointAt(c), e.PointAt(major), e.PointAt(c + side), aux);
        out["ellipse"] = ellipse.value;
    } else {
        Fail(std::format("Неизвестный тип «{}» (line, centerline, polyline, rectangle, circle, arc, slot, polygon, point, spline, "
                         "ellipse)",
                         type));
    }
    if (!names.empty()) out["dimensions"] = std::move(names);
    return out;
}

Json Create(AppContext& app, const Json& args) {
    Session& s = ActivePart(app);
    std::optional<ShownPlane> chosen;
    doc::SketchPlacement placement;
    if (Has(args, "face")) {   // a flat face of the body; the sketch moves with it on rebuild
        const kernel::FaceInfo& face = FaceArg(s, args["face"], "face");
        if (face.kind != doc::SurfaceKind::Plane) Fail("Эскиз ставится только на плоскую грань");
        const Frame frame = doc::PlaneFrame(face.ref);
        placement = {face.body, frame, true, face.ref};
        chosen = ShownPlane{face.body, frame, {}, 0, std::format("грань {}", face.index)};
    } else {
        const std::string plane = Text(args, "plane", "XY");
        for (const ShownPlane& p : OriginPlanes(s))
            if (p.label == plane || std::to_string(p.id.value) == plane) chosen = p;
        for (const ShownPlane& p : WorkPlanes(s))
            if (p.label == plane || std::to_string(p.id.value) == plane) chosen = p;
        if (!chosen) Fail(std::format("Нет плоскости «{}» (XY, XZ, YZ или имя/id рабочей плоскости)", plane));
        placement = {chosen->id, chosen->frame, false, {}};
    }

    auto f       = std::make_unique<doc::SketchFeature>();
    f->id        = s.doc.Ids().Next();
    f->name      = Has(args, "name") ? Text(args, "name") : s.doc.UniqueFeatureName(Tr("Эскиз"));
    f->placement = placement;
    f->sketch.EnsureOrigin(s.doc.Ids());
    const ObjectId id = f->id;
    s.Execute(doc::AddFeature(std::move(f), s.doc.EndMarker()));
    Json out           = AfterChange(s, id);
    const Frame& frame = chosen->frame;
    out["plane"] = {{"name", chosen->label},
                    {"origin_in_model", {frame.origin.x, frame.origin.y, frame.origin.z}},
                    {"sketch_x_in_model", {frame.xDir.x, frame.xDir.y, frame.xDir.z}},
                    {"sketch_y_in_model", {frame.YDir().x, frame.YDir().y, frame.YDir().z}},
                    {"normal", {frame.normal.x, frame.normal.y, frame.normal.z}}};
    return out;
}

Json Add(AppContext& app, const Json& args) {
    Session&    s     = ActivePart(app);
    SketchEdit  edit(s, SketchOf(s, args));
    const Json& items = Required(args, "items");
    if (!items.is_array() || items.empty()) Fail("«items» — непустой список фигур");
    Json made = Json::array();
    for (const Json& item : items) {
        if (!item.is_object()) Fail("Элемент «items» — не объект");
        made.push_back(AddItem(edit, item));
    }
    Json out    = edit.Commit(Tr("Изменить эскиз"));
    out["made"] = std::move(made);
    return out;
}

Json Constrain(AppContext& app, const Json& args) {
    Session&   s = ActivePart(app);
    SketchEdit edit(s, SketchOf(s, args));
    Constraint c;
    if (!doc::ConstraintTypeFromId(Text(args, "type"), c.type)) Fail(std::format("Нет связи «{}»", Text(args, "type")));
    const Json& refs = Required(args, "refs");
    if (!refs.is_array() || refs.empty() || refs.size() > 3) Fail("«refs» — от одного до трёх id элементов эскиза");
    for (std::size_t i = 0; i < refs.size(); ++i) {
        c.refs[i] = ObjectId{refs[i].get<std::uint64_t>()};
        if (edit.Sk().KindOf(c.refs[i]) == sketch::EntityKind::None) Fail(std::format("В эскизе нет элемента {}", c.refs[i].value));
    }
    if (c.type == ConstraintType::Fix) c.fixedAt = edit.Sk().PositionOf(c.refs[0]);
    ObjectId added;
    if (sketch::IsDimensional(c.type)) {
        const Quantity q = sketch::IsAngular(c.type) ? Quantity::Angle : Quantity::Length;
        added = edit.Dimension(c.type, c.refs[0], c.refs[1], 0.0, {8, 8}, q);
        sketch::Constraint* made = edit.Sk().FindConstraint(added);
        made->refs               = c.refs;
        made->parameter.expression = Expression(args, "value");
        made->reference          = Flag(args, "reference", false);
    } else {
        added = edit.Sk().AddConstraint(edit.Ids(), c);
    }
    const std::string name = edit.NameOf(added);
    Json out = edit.Commit(Tr("Изменить эскиз"));
    out["constraint"] = added.value;
    if (!name.empty()) out["parameter"] = name;
    return out;
}

Json Remove(AppContext& app, const Json& args) {
    Session&    s = ActivePart(app);
    SketchEdit  edit(s, SketchOf(s, args));
    const Json& ids = Required(args, "entities");
    if (!ids.is_array() || ids.empty()) Fail("«entities» — непустой список id");
    for (const Json& v : ids) {
        const ObjectId id{v.get<std::uint64_t>()};
        if (edit.Sk().KindOf(id) == sketch::EntityKind::None) Fail(std::format("В эскизе нет элемента {}", id.value));
        edit.Sk().Remove(id);
    }
    return edit.Commit(Tr("Изменить эскиз"));
}

std::vector<ObjectId> EntitiesArg(SketchEdit& e, const Json& args) {
    const Json& list = Required(args, "entities");
    if (!list.is_array() || list.empty()) Fail("«entities» — непустой список id элементов эскиза");
    std::vector<ObjectId> out;
    for (const Json& v : list) {
        if (!v.is_number_unsigned()) Fail("В «entities» — id (числа)");
        const ObjectId id{v.get<std::uint64_t>()};
        if (e.Sk().KindOf(id) == sketch::EntityKind::None) Fail(std::format("В эскизе нет элемента {}", id.value));
        out.push_back(id);
    }
    return out;
}

ObjectId CurveArg(SketchEdit& e, const Json& args, const char* key) {
    const ObjectId id = Id(args, key);
    if (e.Sk().KindOf(id) == sketch::EntityKind::None) Fail(std::format("В эскизе нет элемента {}", id.value));
    return id;
}

Json Ids(const std::vector<ObjectId>& ids) {
    Json out = Json::array();
    for (ObjectId id : ids) out.push_back(id.value);
    return out;
}

/// The editing tools of the sketch ribbon, by their geometry (sketch/Trim.h,
/// Fillet.h, Offset.h, Mirror.h, Move.h): one undo step each.
Json Edit(AppContext& app, const Json& args) {
    Session&        s = ActivePart(app);
    SketchEdit      edit(s, SketchOf(s, args));
    sketch::Sketch& sk = edit.Sk();
    static const std::vector<std::string> kOps = {"fillet", "offset", "trim", "extend", "split", "mirror",
                                                  "move", "copy", "rotate", "scale"};
    const std::size_t op = Choice(args, "operation", kOps, kOps.size());
    if (op >= kOps.size()) Fail("Нужна «operation»: fillet, offset, trim, extend, split, mirror, move, copy, rotate, scale");
    Json made = {{"operation", kOps[op]}};
    switch (op) {
    case 0: {   // fillet: the corner point of two lines
        const ObjectId corner = CurveArg(edit, args, "point");
        const double   r      = Number(args, "radius");
        if (r <= 0) Fail("radius — больше нуля");
        const std::optional<ObjectId> arc = sketch::FilletCorner(sk, edit.Ids(), corner, r);
        if (!arc) Fail("Скругление не помещается: в точке должны сходиться два отрезка, достаточно длинных для этого радиуса");
        made["arc"] = arc->value;
        if (Flag(args, "dimensions", true)) {
            const ObjectId d = edit.Dimension(ConstraintType::Radius, *arc, {}, r, Vec2{1, 1}.Normalized() * (r + 8));
            made["dimension"] = edit.NameOf(d);
        }
        break;
    }
    case 1: {   // offset: the chain through a curve, to the side of `toward`
        const ObjectId curve = CurveArg(edit, args, "curve");
        const double   d     = Number(args, "distance");
        if (d <= 0) Fail("distance — больше нуля");
        const std::optional<sketch::Chain> chain = sketch::ChainOf(sk, curve);
        if (!chain) Fail("Эту кривую нельзя сместить (проекция или не отрезок / дуга / окружность)");
        const int side = Has(args, "toward") ? sketch::SideOf(sk, *chain, Point(args, "toward", std::nullopt)) : 1;
        const std::optional<sketch::OffsetCurves> curves = sketch::OffsetChain(sk, edit.Ids(), *chain, side * d);
        if (!curves) Fail("Смещение не строится: дуга схлопнулась бы или соседние кривые копии не сходятся");
        made["curves"] = Ids(curves->curves);
        // The first distance drives, the rest follow it (as the program's tool does).
        std::string driver;
        Json names = Json::array();
        for (const auto& [point, line] : curves->distances) {
            const ObjectId c = edit.Dimension(ConstraintType::PointLineDistance, point, line, d, {8, 8});
            if (driver.empty()) driver = edit.NameOf(c);
            else sk.FindConstraint(c)->parameter.expression = driver;
            names.push_back(edit.NameOf(c));
        }
        if (!names.empty()) made["dimensions"] = std::move(names);
        break;
    }
    case 2: {   // trim: the piece of `curve` around `at`
        const ObjectId curve = CurveArg(edit, args, "curve");
        const std::optional<sketch::TrimSpan> span = sketch::FindTrimSpan(sk, curve, Point(args, "at", std::nullopt));
        if (!span || !sketch::Trim(sk, edit.Ids(), *span)) Fail("Обрезать нечего: кривую ничего не пересекает");
        break;
    }
    case 3: {   // extend: the end of a line nearer `at`
        const ObjectId line = CurveArg(edit, args, "curve");
        const std::optional<sketch::Extension> ext = sketch::FindExtension(sk, line, Point(args, "at", std::nullopt));
        if (!ext) Fail("Удлинять некуда: впереди нет кривой, или конец общий с другой кривой");
        sketch::Extend(sk, edit.Ids(), *ext);
        made["to"] = {ext->to.x, ext->to.y};
        break;
    }
    case 4: {   // split a line or an arc at `at`
        const ObjectId curve = CurveArg(edit, args, "curve");
        const std::optional<ObjectId> point = sketch::Split(sk, edit.Ids(), curve, Point(args, "at", std::nullopt));
        if (!point) Fail("Разбить можно отрезок или дугу, не у самого конца");
        made["point"] = point->value;
        break;
    }
    case 5: {   // mirror over a line
        const std::vector<ObjectId> entities = EntitiesArg(edit, args);
        const ObjectId axis = CurveArg(edit, args, "axis");
        if (!sk.FindLine(axis)) Fail("«axis» — отрезок (ось симметрии)");
        made["made"] = Ids(sketch::MirrorEntities(sk, edit.Ids(), entities, axis));
        break;
    }
    case 6:
    case 7:
    case 8: {   // move / copy (by), rotate (center, angle)
        const std::vector<ObjectId> entities = EntitiesArg(edit, args);
        sketch::Motion motion;
        if (op == 8) {
            motion.centre = Point(args, "center", Vec2{});
            motion.angle  = Number(args, "angle") * kPi / 180.0;
        } else {
            const Vec2 by = Point(args, "by", std::nullopt);
            motion.offset = by;
        }
        if (op == 7) {
            const std::vector<ObjectId> copies = sketch::CopyEntities(sk, edit.Ids(), entities, motion);
            if (copies.empty()) Fail("Копировать нечего: среди элементов нет кривых");
            made["made"] = Ids(copies);
        } else if (!sketch::MoveEntities(sk, edit.Ids(), entities, motion)) {
            Fail("Ничего не сдвинулось: точки закреплены (начало координат, проекции, связь fix)");
        }
        break;
    }
    case 9: {   // scale about a centre
        const std::vector<ObjectId> entities = EntitiesArg(edit, args);
        const double factor = Number(args, "factor");
        if (factor <= 0) Fail("factor — больше нуля");
        if (!sketch::ScaleEntities(sk, entities, Point(args, "center", Vec2{}), factor)) Fail("Масштабировать нечего");
        break;
    }
    default: break;
    }
    Json out = edit.Commit(Tr("Изменить эскиз"));
    out["made"] = std::move(made);
    return out;
}

/// Model edges into the sketch, following the model (the program's
/// «Проецировать геометрию», linked): the curves move when the edges do.
Json Project(AppContext& app, const Json& args) {
    Session&        s = ActivePart(app);
    SketchEdit      edit(s, SketchOf(s, args));
    sketch::Sketch& sk = edit.Sk();
    const int index = s.doc.IndexOf(edit.Feature().id);
    const Json& list = Required(args, "edges");
    if (!list.is_array() || list.empty()) Fail("«edges» — номера рёбер из part.edges");
    // Neighbouring edges share their ends: one projected point for both.
    const auto point = [&](Vec2 at) {
        for (const sketch::Point& p : sk.points)
            if (p.projected && (p.pos - at).Length() < 1e-6) return p.id;
        const ObjectId id = sk.AddPoint(edit.Ids(), at);
        sk.FindPoint(id)->projected = true;
        return id;
    };
    Json curves = Json::array();
    for (const Json& v : list) {
        const kernel::EdgeInfo& edge = EdgeArg(s, v, "edges");
        const Expected<kernel::ProjectedCurve> pc = s.model.ProjectEdge(edge.ref, edit.Feature().placement.frame, static_cast<std::size_t>(index));
        if (!pc) Fail(std::format("Ребро {} не проецируется: {}", edge.index, Tr(pc.GetError().message)));
        ObjectId curve;
        switch (pc->kind) {
        case kernel::ProjectedCurve::Kind::Line:
            if ((pc->end - pc->start).Length() < 1e-6) continue;   // seen end-on: a point, nothing to draw
            curve = sk.AddLine(edit.Ids(), point(pc->start), point(pc->end));
            sk.FindLine(curve)->projected = true;
            break;
        case kernel::ProjectedCurve::Kind::Circle:
            curve = sk.AddCircle(edit.Ids(), point(pc->centre), pc->radius);
            sk.FindCircle(curve)->projected = true;
            break;
        case kernel::ProjectedCurve::Kind::Arc:
            curve = sk.AddArc(edit.Ids(), point(pc->centre), point(pc->start), point(pc->end));
            for (auto& a : sk.arcs)
                if (a.id == curve) a.projected = true;
            break;
        case kernel::ProjectedCurve::Kind::Ellipse:
            curve = sk.AddEllipse(edit.Ids(), point(pc->centre), point(pc->start), point(pc->end));
            break;
        case kernel::ProjectedCurve::Kind::Spline:
            curve = kernel::AddProjectedSpline(sk, edit.Ids(), *pc, point);
            if (!curve.IsValid()) continue;
            break;
        }
        edit.Feature().projections.push_back({edge.ref, curve, {}});
        curves.push_back({{"edge", edge.index}, {"curve", curve.value}});
    }
    Json out = edit.Commit(Tr("Изменить эскиз"));
    out["projected"] = std::move(curves);
    return out;
}

Json Get(AppContext& app, const Json& args) {
    Session& s = ActivePart(app);
    Rebuild(s);
    const doc::SketchFeature& f  = SketchOf(s, args);
    const sketch::Sketch&     sk = f.sketch;
    Json out = Describe(f);
    const auto xy = [](Vec2 p) { return Json::array({std::round(p.x * 1e6) / 1e6, std::round(p.y * 1e6) / 1e6}); };
    out["degrees_of_freedom"] = f.analysis.degreesOfFreedom;
    Json pts = Json::array(), lines = Json::array(), circles = Json::array(), arcs = Json::array(), cons = Json::array();
    for (const auto& p : sk.points) {
        Json j = {{"id", p.id.value}, {"at", xy(p.pos)}};
        if (p.isOrigin) j["origin"] = true;
        pts.push_back(std::move(j));
    }
    for (const auto& l : sk.lines) {
        Json j = {{"id", l.id.value}, {"start", l.start.value}, {"end", l.end.value},
                  {"from", xy(sk.PositionOf(l.start))}, {"to", xy(sk.PositionOf(l.end))}};
        if (l.construction) j["construction"] = true;
        if (l.centerline) j["centerline"] = true;
        lines.push_back(std::move(j));
    }
    for (const auto& c : sk.circles)
        circles.push_back({{"id", c.id.value}, {"center", c.center.value}, {"at", xy(sk.PositionOf(c.center))}, {"radius", c.radius}});
    for (const auto& a : sk.arcs)
        arcs.push_back({{"id", a.id.value}, {"center", a.center.value}, {"start", a.start.value}, {"end", a.end.value},
                        {"radius", sk.RadiusOf(a.id)}});
    for (const auto& c : sk.constraints) {
        Json j = {{"id", c.id.value}, {"type", std::string(doc::ConstraintTypeId(c.type))}, {"refs", Json::array()}};
        for (ObjectId r : c.refs)
            if (r) j["refs"].push_back(r.value);
        if (sketch::IsDimensional(c.type)) j["parameter"] = {{"name", c.parameter.name}, {"expression", c.parameter.expression}};
        if (std::find(f.analysis.failing.begin(), f.analysis.failing.end(), c.id) != f.analysis.failing.end()) j["conflict"] = true;
        cons.push_back(std::move(j));
    }
    out["points"] = std::move(pts);
    out["lines"] = std::move(lines);
    out["circles"] = std::move(circles);
    out["arcs"] = std::move(arcs);
    out["constraints"] = std::move(cons);
    if (!sk.splines.empty()) {
        Json splines = Json::array();
        for (const auto& sp : sk.splines) {
            Json j = {{"id", sp.id.value}, {"points", Json::array()}, {"at", Json::array()}, {"interpolate", sp.interpolate}};
            for (ObjectId p : sp.points) {
                j["points"].push_back(p.value);
                j["at"].push_back(xy(sk.PositionOf(p)));
            }
            if (sp.Closed()) j["closed"] = true;
            if (sp.construction) j["construction"] = true;
            splines.push_back(std::move(j));
        }
        out["splines"] = std::move(splines);
    }
    if (!sk.ellipses.empty()) {
        Json ellipses = Json::array();
        for (const auto& el : sk.ellipses) {
            Json j = {{"id", el.id.value}, {"center", el.center.value}, {"major", el.major.value}, {"minor", el.minor.value},
                      {"at", xy(sk.PositionOf(el.center))}, {"major_at", xy(sk.PositionOf(el.major))}, {"minor_at", xy(sk.PositionOf(el.minor))}};
            if (el.construction) j["construction"] = true;
            ellipses.push_back(std::move(j));
        }
        out["ellipses"] = std::move(ellipses);
    }
    if (!sk.texts.empty()) out["texts"] = sk.texts.size();
    Json regions = Json::array();
    if (const auto* rs = s.model.Regions(f.id))
        for (const auto& r : *rs) regions.push_back({{"inside", xy(r.sample)}, {"holes", r.loops.size() - 1}, {"automatic", r.automatic}});
    out["profiles"] = std::move(regions);
    return out;
}

Json ItemSchema() {
    return {{"type", "object"},
            {"description",
             "Фигура. type: line {from,to} | centerline {from,to} — осевая для вращения | polyline {points,closed} | "
             "rectangle {corner,opposite} или {center,width,height} | circle {center, radius или diameter} | "
             "arc {center,start,end} — против часовой | slot {from,to,width} | polygon {center,sides,radius,inscribed} | "
             "point {at} | spline {points, interpolate — через точки (по умолчанию) или по управляющим, closed} | "
             "ellipse {center, major — конец большой оси, minor_radius}. "
             "Общие: construction (вспомогательная), dimensions (по умолчанию true — проставить размеры, "
             "они станут параметрами d1, d2…). Точки с одинаковыми координатами объединяются, так контуры замыкаются."},
            {"properties",
             {{"type", {{"type", "string"}, {"enum", {"line", "centerline", "polyline", "rectangle", "circle", "arc", "slot", "polygon", "point", "spline", "ellipse"}}}},
              {"at", PointArg("point: где.")}, {"interpolate", BoolArg("spline: через точки (по умолчанию да).")},
              {"major", PointArg("ellipse: конец большой оси.")}, {"minor_radius", NumberArg("ellipse: малая полуось, мм.")},
              {"from", PointArg("Начало.")}, {"to", PointArg("Конец.")}, {"points", {{"type", "array"}, {"items", PointArg("Точка.")}}},
              {"closed", BoolArg("polyline, spline: замкнуть.")}, {"corner", PointArg("Угол.")}, {"opposite", PointArg("Противоположный угол.")},
              {"center", PointArg("Центр.")}, {"width", NumberArg("Ширина, мм.")}, {"height", NumberArg("Высота, мм.")},
              {"radius", NumberArg("Радиус, мм.")}, {"diameter", NumberArg("Диаметр, мм.")}, {"start", PointArg("Начало дуги.")},
              {"end", PointArg("Конец дуги (ставится на радиус).")}, {"sides", IntegerArg("Число сторон.")},
              {"inscribed", BoolArg("polygon: вписанный (по умолчанию) или описанный.")},
              {"construction", BoolArg("Вспомогательная геометрия.")}, {"dimensions", BoolArg("Проставить размеры (по умолчанию true).")}}},
            {"required", {"type"}}};
}

} // namespace

void AddSketchCommands(Commands& out) {
    out.push_back({"sketch.create",
                   "Создаёт пустой эскиз на плоскости начала координат (XY — вид сверху, XZ — спереди, YZ — справа), "
                   "на рабочей плоскости или на плоской грани детали (face — номер из part.faces). Возвращает id эскиза, "
                   "где лежит его начало (0,0) и как оси эскиза лежат в модели: точка эскиза (u,v) = origin + u·x + v·y. "
                   "Модель — Z вверх.",
                   Schema({{"plane", StringArg("XY, XZ, YZ или имя/id рабочей плоскости; по умолчанию XY.")},
                           {"face", IntegerArg("Плоская грань (номер из part.faces) вместо плоскости.")},
                           {"name", StringArg("Имя эскиза; по умолчанию «ЭскизN».")}}),
                   true, &Create});
    out.push_back({"sketch.add",
                   "Добавляет в эскиз фигуры одним шагом отмены. Координаты — мм в системе эскиза, начало эскиза (0,0) — "
                   "проекция начала координат. По умолчанию проставляет размеры — они становятся параметрами (d1…), "
                   "их потом меняет part.set_parameter. Возвращает id созданного и имена размеров.",
                   Schema({{"sketch", IdArg("id эскиза.")}, {"items", {{"type", "array"}, {"items", ItemSchema()}}}}, {"sketch", "items"}),
                   true, &Add});
    out.push_back({"sketch.constrain",
                   "Добавляет связь или размер между элементами эскиза (id из sketch.get). Геометрические: coincident, "
                   "horizontal, vertical, parallel, collinear (два отрезка на одной прямой), perpendicular, equal, fix, pointOnCurve, "
                   "midpoint, tangent, concentric, "
                   "symmetric (точка, точка, линия). Размеры (нужен value): distance, distanceX, distanceY (две точки), "
                   "length (отрезок), pointLineDistance, radius, diameter, angle (два отрезка).",
                   Schema({{"sketch", IdArg("id эскиза.")},
                           {"type", StringArg("Тип связи (см. описание).")},
                           {"refs", {{"type", "array"}, {"items", {{"type", "integer"}}}, {"description", "id элементов эскиза, 1–3."}}},
                           {"value", ExpressionArg("Значение размера.")},
                           {"reference", BoolArg("Справочный размер (ничего не задаёт).")}},
                          {"sketch", "type", "refs"}),
                   true, &Constrain});
    out.push_back({"sketch.remove", "Удаляет элементы эскиза (точку, кривую, связь) вместе с тем, что от них зависит.",
                   Schema({{"sketch", IdArg("id эскиза.")},
                           {"entities", {{"type", "array"}, {"items", {{"type", "integer"}}}, {"description", "id элементов."}}}},
                          {"sketch", "entities"}),
                   true, &Remove});
    out.push_back({"sketch.edit",
                   "Правка эскиза инструментами ленты (id из sketch.get, точки — мм эскиза). operation: "
                   "fillet {point — угол двух отрезков, radius} | offset {curve, distance, toward — точка, с какой стороны} — "
                   "вся цепочка через кривую, с размерами | trim {curve, at — точка на удаляемом куске} | "
                   "extend {curve — отрезок, at — у какого конца} | split {curve, at} | mirror {entities, axis — отрезок} | "
                   "move {entities, by: [dx, dy]} | copy {entities, by} | rotate {entities, center, angle — градусы против часовой} | "
                   "scale {entities, center, factor}.",
                   Schema({{"sketch", IdArg("id эскиза.")},
                           {"operation", EnumArg("Что сделать.", {"fillet", "offset", "trim", "extend", "split", "mirror", "move", "copy", "rotate", "scale"})},
                           {"point", IntegerArg("fillet: id точки угла.")},
                           {"radius", NumberArg("fillet: радиус, мм.")},
                           {"curve", IntegerArg("offset, trim, extend, split: id кривой.")},
                           {"distance", NumberArg("offset: расстояние, мм.")},
                           {"toward", PointArg("offset: точка с той стороны, куда смещать.")},
                           {"at", PointArg("trim, extend, split: точка на кривой.")},
                           {"entities", {{"type", "array"}, {"items", {{"type", "integer"}}}, {"description", "mirror, move, copy, rotate, scale: id элементов."}}},
                           {"axis", IntegerArg("mirror: id отрезка-оси.")},
                           {"by", PointArg("move, copy: сдвиг [dx, dy], мм.")},
                           {"center", PointArg("rotate, scale: центр, мм (по умолчанию начало эскиза).")},
                           {"angle", NumberArg("rotate: угол, градусы.")},
                           {"factor", NumberArg("scale: множитель.")},
                           {"dimensions", BoolArg("fillet: проставить радиус (по умолчанию да).")}},
                          {"sketch", "operation"}),
                   true, &Edit});
    out.push_back({"sketch.project",
                   "Проецирует рёбра тела (номера из part.edges) в эскиз как связанную геометрию: кривые следуют за рёбрами, "
                   "когда модель меняется. Рёбра — тел, построенных до этого эскиза.",
                   Schema({{"sketch", IdArg("id эскиза.")},
                           {"edges", IndexListArg("Номера рёбер из part.edges.")}},
                          {"sketch", "edges"}),
                   true, &Project});
    out.push_back({"sketch.get",
                   "Решённый эскиз: точки, отрезки, окружности, дуги с id и координатами; связи и размеры (конфликтующие "
                   "помечены), число степеней свободы, замкнутые контуры (profiles: точка внутри каждого — для выдавливания).",
                   Schema({{"sketch", IdArg("id эскиза.")}}, {"sketch"}), false, &Get});
}

} // namespace bz::app::scripting
