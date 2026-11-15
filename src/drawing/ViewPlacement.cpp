#include "drawing/ViewPlacement.h"

#include <algorithm>

namespace bz::drawing {

namespace {

bool Overlaps(const Box2& a, const Box2& b) { return a.min.x < b.max.x && b.min.x < a.max.x && a.min.y < b.max.y && b.min.y < a.max.y; }

} // namespace

Box2 Extent(ViewDirection direction, const Box3& m) {
    Box2 box;
    for (double x : {m.min.x, m.max.x})
        for (double y : {m.min.y, m.max.y})
            for (double z : {m.min.z, m.max.z}) box.Include(ProjectPoint(direction, {x, y, z}));
    return box;
}

Box2 Extent(const View& view, const Box3& m) {
    Box2 box;
    for (double x : {m.min.x, m.max.x})
        for (double y : {m.min.y, m.max.y})
            for (double z : {m.min.z, m.max.z}) box.Include(ProjectPoint(view, {x, y, z}));
    return box;
}

bool Room::Fits(const Box2& b) const {
    if (b.min.x < area.min.x || b.max.x > area.max.x || b.min.y < area.min.y || b.max.y > area.max.y) return false;
    return std::none_of(taken.begin(), taken.end(), [&](const Box2& t) { return Overlaps(t, b); });
}

Room RoomOf(const SheetLayout& l, const std::vector<Box2>& taken) {
    constexpr double margin = 10;
    Room r;
    r.area.Include(l.frame.min + Vec2{margin, margin});
    r.area.Include(l.frame.max - Vec2{margin, margin});
    Box2 block;
    block.Include(l.titleBlock.min - Vec2{5, 5});
    block.Include(l.titleBlock.max + Vec2{5, 5});
    r.taken.push_back(block);
    Box2 graph26;   // the designation turned by 180° at the top left, 70 × 14
    graph26.Include({l.frame.min.x, l.frame.max.y - 19});
    graph26.Include({l.frame.min.x + 75, l.frame.max.y});
    r.taken.push_back(graph26);
    r.taken.insert(r.taken.end(), taken.begin(), taken.end());
    return r;
}

std::optional<StandardPlacement> PlaceStandardViews(const Room& room, const Box3& model) {
    const Box2 f = Extent(ViewDirection::Front, model), t = Extent(ViewDirection::Top, model), l = Extent(ViewDirection::Left, model);
    constexpr double gap = 20;
    const double aw = room.area.max.x - room.area.min.x, ah = room.area.max.y - room.area.min.y;
    for (const Scale& sc : StandardScales()) {
        const double s = sc.Value();
        const double fw = (f.max.x - f.min.x) * s, fh = (f.max.y - f.min.y) * s;
        const double th = (t.max.y - t.min.y) * s, lw = (l.max.x - l.min.x) * s, lh = (l.max.y - l.min.y) * s;
        const double w = fw + gap + lw, h = fh + gap + th;
        if (w > aw || h > ah) continue;
        for (const Vec2 corner : {Vec2{room.area.min.x + (aw - w) / 2, room.area.max.y - (ah - h) / 2}, Vec2{room.area.min.x, room.area.max.y}}) {
            Box2 fb, tb, lb;
            fb.Include(corner);
            fb.Include(corner + Vec2{fw, -fh});
            tb.Include({corner.x, corner.y - fh - gap});
            tb.Include({corner.x + fw, corner.y - fh - gap - th});
            lb.Include({corner.x + fw + gap, corner.y});
            lb.Include({corner.x + fw + gap + lw, corner.y - lh});
            if (!room.Fits(fb) || !room.Fits(tb) || !room.Fits(lb)) continue;
            StandardPlacement p{sc, {corner.x - f.min.x * s, corner.y - f.max.y * s}, {}, {}};
            p.top  = {p.front.x, tb.max.y - t.max.y * s};
            p.left = {lb.min.x - l.min.x * s, p.front.y};
            return p;
        }
    }
    return std::nullopt;
}

std::optional<Vec2> FreeSpot(const Room& room, Vec2 size) {
    for (double y = room.area.max.y; y - size.y >= room.area.min.y; y -= 5)
        for (double x = room.area.max.x; x - size.x >= room.area.min.x; x -= 5) {
            Box2 b;
            b.Include({x - size.x, y - size.y});
            b.Include({x, y});
            if (room.Fits(b)) return Vec2{x - size.x, y - size.y};
        }
    return std::nullopt;
}

} // namespace bz::drawing
