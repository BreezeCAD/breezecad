#include "drawing/ViewLayout.h"

#include <algorithm>
#include <cmath>

namespace bz::drawing {

namespace {

constexpr double kEmptyViewSize = 40.0;   // mm: the box of a view without lines, so it can be seen and picked
constexpr double kViewGap       = 20.0;   // mm between the standard views: room for dimensions (this program's choice)
constexpr double kFrameGap      = 10.0;   // mm from the frame and the title block
constexpr double kSmallShare    = 1.0 / 3; // of the room: views smaller than this at 1:1 are enlarged (this program's choice)

double Length(const Polyline& line) {
    double sum = 0;
    for (std::size_t i = 1; i < line.size(); ++i) sum += (line[i] - line[i - 1]).Length();
    return sum;
}

Polyline OnSheet(const Polyline& line, Vec2 at, double factor) {
    Polyline out;
    out.reserve(line.size());
    for (const Vec2 p : line) out.push_back(at + p * factor);
    return out;
}

Box BoxOf(const ViewLines& lines, Vec2 at, double factor) { return {at + lines.min * factor, at + lines.max * factor}; }

bool Overlap(const Box& a, const Box& b) {
    return a.min.x < b.max.x && b.min.x < a.max.x && a.min.y < b.max.y && b.min.y < a.max.y;
}

bool Inside(const Box& inner, const Box& outer) {
    return inner.min.x >= outer.min.x && inner.max.x <= outer.max.x && inner.min.y >= outer.min.y && inner.max.y <= outer.max.y;
}

Box Grown(const Box& box, double by) { return {box.min - Vec2{by, by}, box.max + Vec2{by, by}}; }

} // namespace

std::vector<Polyline> Dashed(const Polyline& line, double dash, double gap) {
    std::vector<Polyline> out;
    const double length = Length(line);
    if (length <= 0) return out;
    // n dashes and n - 1 gaps fill the line; the dash length gives way, the gap does not.
    const int count = static_cast<int>(std::lround((length + gap) / (dash + gap)));
    if (count < 2) {
        out.push_back(line);
        return out;
    }
    const double fitted = (length - (count - 1) * gap) / count;
    for (int i = 0; i < count; ++i) {
        const double from = i * (fitted + gap);
        Polyline piece = Slice(line, from, from + fitted);
        if (piece.size() >= 2) out.push_back(std::move(piece));
    }
    return out;
}

ViewPlace PlaceOf(const Sheet& sheet, const ModelView& view) {
    ViewPlace place{view.at, view.scale.value_or(Scale{})};
    if (!view.parent.IsValid() || view.parent == view.id) return place;
    const Item* item = FindViewItem(sheet, view.parent);
    const std::optional<ModelView> parent = item ? ReadModelView(*item) : std::nullopt;
    if (!parent) return place;
    // One step only: a projected view hangs on a main view, never on another projected one.
    place.scale = parent->scale.value_or(place.scale);
    if (view.projection == ViewProjection::Top) place.at.x = parent->at.x;
    if (view.projection == ViewProjection::Left) place.at.y = parent->at.y;
    return place;
}

Vec2 KeepLink(const Sheet& sheet, const ModelView& view, Vec2 at) {
    ModelView moved = view;
    moved.at = at;
    return PlaceOf(sheet, moved).at;
}

void MoveViewTo(Sheet& sheet, ObjectId id, Vec2 at) {
    Item* item = FindViewItem(sheet, id);
    std::optional<ModelView> view = item ? ReadModelView(*item) : std::nullopt;
    if (!view) return;
    const Vec2 from = PlaceOf(sheet, *view).at;
    view->at = KeepLink(sheet, *view, at);
    const Vec2 shift = view->at - from;
    StoreModelView(*item, *view);
    for (Item& other : sheet.views) {
        std::optional<ModelView> child = ReadModelView(other);
        if (!child || child->parent != id || child->id == id) continue;
        child->at = child->at + shift;
        StoreModelView(other, *child);
    }
}

std::vector<PlacedView> LayoutViews(const Sheet& sheet, const ViewImages& images) {
    std::vector<PlacedView> placed;
    for (const ModelView& view : ModelViewsOf(sheet)) {
        PlacedView p;
        const ViewPlace place = PlaceOf(sheet, view);
        p.view       = view;
        p.view.at    = place.at;
        p.view.scale = place.scale;
        p.scale      = place.scale;
        const double factor = place.scale.Factor();
        const auto image = images.find(view.id);
        if (image == images.end() || !image->second || image->second->Empty()) {
            const Vec2 half{kEmptyViewSize / 2, kEmptyViewSize / 2};
            p.box = {place.at - half, place.at + half};
            placed.push_back(std::move(p));
            continue;
        }
        const ViewLines& lines = *image->second;
        p.computed = true;
        p.box      = BoxOf(lines, place.at, factor);
        // Hidden lines first: a seen line drawn after them lies on top where they touch.
        if (view.hiddenLines)
            for (const Polyline& line : lines.hidden)
                for (Polyline& piece : Dashed(OnSheet(line, place.at, factor))) p.paths.push_back({std::move(piece), LineWeight::Thin});
        for (const Polyline& line : lines.visible) p.paths.push_back({OnSheet(line, place.at, factor), LineWeight::Thick});
        placed.push_back(std::move(p));
    }
    return placed;
}

std::optional<StandardViewsPlan> PlanStandardViews(const SheetLayout& sheet, const ViewLines& front, const ViewLines& top,
                                                   const ViewLines& left) {
    if (front.Empty()) return std::nullopt;
    const Box room = Grown(sheet.frame, -kFrameGap);
    const Box block = Grown(sheet.titleBlock, kFrameGap);
    const double roomWidth = room.max.x - room.min.x, roomHeight = room.max.y - room.min.y;
    // Life size is the scale to prefer: a part is enlarged only when its views at 1:1
    // would be lost on the sheet — less than a third of the room both ways.
    const bool smallAtLifeSize = front.Width() + kViewGap + left.Width() < roomWidth * kSmallShare &&
                                 front.Height() + kViewGap + top.Height() < roomHeight * kSmallShare;
    for (const Scale scale : StandardScales()) {
        const double f = scale.Factor();
        if (f > 1 && !smallAtLifeSize) continue;
        const double width = front.Width() * f + kViewGap + left.Width() * f;
        const double height = front.Height() * f + kViewGap + top.Height() * f;
        if (width > roomWidth || height > roomHeight) continue;
        // The three in the middle of the frame; if the view from above then runs into the
        // title block, everything goes up above the block.
        const double x0 = (room.min.x + room.max.x - width) / 2;
        for (const double floor : {room.min.y, block.max.y}) {
            if (height > room.max.y - floor) continue;
            const double yTop = (floor + room.max.y + height) / 2;
            StandardViewsPlan plan;
            plan.scale = scale;
            plan.front = {x0 - front.min.x * f, yTop - front.max.y * f};
            const Box frontBox = BoxOf(front, plan.front, f);
            plan.top  = {plan.front.x, frontBox.min.y - kViewGap - top.max.y * f};
            plan.left = {frontBox.max.x + kViewGap - left.min.x * f, plan.front.y};
            const Box topBox = BoxOf(top, plan.top, f), leftBox = BoxOf(left, plan.left, f);
            bool fits = true;
            for (const Box& box : {frontBox, topBox, leftBox}) fits = fits && Inside(box, room) && !Overlap(box, block);
            if (fits) return plan;
        }
    }
    return std::nullopt;
}

} // namespace bz::drawing
