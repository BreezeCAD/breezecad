#include "drawing/ViewLines.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <utility>

namespace bz::drawing {

namespace {

struct Segment {
    Vec2 a, b;
};

double DistanceTo(const Segment& s, Vec2 p) {
    const Vec2 d = s.b - s.a;
    const double len2 = d.Dot(d);
    const double t = len2 > 0 ? std::clamp((p - s.a).Dot(d) / len2, 0.0, 1.0) : 0.0;
    return (p - (s.a + d * t)).Length();
}

/// Segments in a uniform grid: only the ones near a point are looked at.
class Cover {
public:
    Cover(Vec2 origin, double cell) : myOrigin(origin), myCell(cell) {}

    void Add(const Polyline& line, double reach) {
        for (std::size_t i = 1; i < line.size(); ++i) {
            const Segment s{line[i - 1], line[i]};
            const int index = static_cast<int>(mySegments.size());
            mySegments.push_back(s);
            myStamp.push_back(0);
            Cells(s, reach, [&](std::int64_t key) { myCells[key].push_back(index); });
        }
    }

    /// Segments whose cells the segment `s` (grown by `reach`) touches, each once.
    template <class F>
    void Near(const Segment& s, double reach, F&& visit) {
        ++myRound;
        Cells(s, reach, [&](std::int64_t key) {
            const auto it = myCells.find(key);
            if (it == myCells.end()) return;
            for (const int index : it->second) {
                if (myStamp[static_cast<std::size_t>(index)] == myRound) continue;
                myStamp[static_cast<std::size_t>(index)] = myRound;
                visit(mySegments[static_cast<std::size_t>(index)]);
            }
        });
    }

    bool Empty() const { return mySegments.empty(); }

private:
    template <class F>
    void Cells(const Segment& s, double reach, F&& visit) const {
        const auto cell = [&](double v, double origin) { return static_cast<std::int64_t>(std::floor((v - origin) / myCell)); };
        const std::int64_t x0 = cell(std::min(s.a.x, s.b.x) - reach, myOrigin.x), x1 = cell(std::max(s.a.x, s.b.x) + reach, myOrigin.x);
        const std::int64_t y0 = cell(std::min(s.a.y, s.b.y) - reach, myOrigin.y), y1 = cell(std::max(s.a.y, s.b.y) + reach, myOrigin.y);
        for (std::int64_t y = y0; y <= y1; ++y)
            for (std::int64_t x = x0; x <= x1; ++x) visit(y * 1000003 + x);   // a clash only costs a few extra checks
    }

    Vec2   myOrigin;
    double myCell;
    std::vector<Segment> mySegments;
    std::vector<std::uint32_t> myStamp;
    std::uint32_t myRound = 0;
    std::unordered_map<std::int64_t, std::vector<int>> myCells;
};

using Interval = std::pair<double, double>;

constexpr double kSameWay = 0.26;   // sin 15°: lines closer in direction than this can lie on each other

/// The part of `c` (as 0…1 along it) within `tolerance` of `s`; false when none.
/// The distance from a point moving along a line to a segment is convex, so
/// that part is one interval: its lowest point, then its two ends.
bool CoveredPart(const Segment& c, const Segment& s, double tolerance, Interval& out) {
    // Only a line going the same way covers: one that crosses or meets at a corner does not.
    const Vec2 dc = c.b - c.a, ds = s.b - s.a;
    const double lengths = dc.Length() * ds.Length();
    if (lengths <= 0 || std::abs(dc.x * ds.y - dc.y * ds.x) > kSameWay * lengths) return false;
    const auto at = [&](double t) { return DistanceTo(s, c.a + (c.b - c.a) * t); };
    double lo = 0, hi = 1;
    for (int i = 0; i < 40; ++i) {
        const double m1 = lo + (hi - lo) / 3, m2 = hi - (hi - lo) / 3;
        if (at(m1) < at(m2)) hi = m2;
        else lo = m1;
    }
    const double lowest = (lo + hi) / 2;
    if (at(lowest) > tolerance) return false;
    const auto edge = [&](double outside) {   // between `outside` (too far) and `lowest` (near)
        double farT = outside, nearT = lowest;
        for (int i = 0; i < 40; ++i) {
            const double m = (farT + nearT) / 2;
            if (at(m) <= tolerance) nearT = m;
            else farT = m;
        }
        return nearT;
    };
    out.first  = at(0) <= tolerance ? 0.0 : edge(0);
    out.second = at(1) <= tolerance ? 1.0 : edge(1);
    return true;
}

/// Parts of the segment `c` covered by `cover`, merged, in order.
std::vector<Interval> CoveredParts(const Segment& c, Cover& cover, double tolerance) {
    std::vector<Interval> parts;
    cover.Near(c, tolerance, [&](const Segment& s) {
        Interval part;
        if (CoveredPart(c, s, tolerance, part)) parts.push_back(part);
    });
    std::sort(parts.begin(), parts.end());
    std::vector<Interval> merged;
    const double length = (c.b - c.a).Length();
    const double join = length > 0 ? tolerance / length : 1.0;   // gaps smaller than the tolerance are no gaps
    for (const Interval& p : parts) {
        if (!merged.empty() && p.first <= merged.back().second + join) merged.back().second = std::max(merged.back().second, p.second);
        else merged.push_back(p);
    }
    return merged;
}

double Length(const Polyline& line) {
    double sum = 0;
    for (std::size_t i = 1; i < line.size(); ++i) sum += (line[i] - line[i - 1]).Length();
    return sum;
}

/// `line` without what `cover` covers: the pieces left, each a polyline.
std::vector<Polyline> Uncovered(const Polyline& line, Cover& cover, double tolerance) {
    std::vector<Polyline> out;
    if (cover.Empty()) {
        out.push_back(line);
        return out;
    }
    // The covered stretches as lengths along the line, joined across its points.
    std::vector<Interval> covered;
    double walked = 0;
    for (std::size_t i = 1; i < line.size(); ++i) {
        const Segment c{line[i - 1], line[i]};
        const double step = (c.b - c.a).Length();
        for (const Interval& part : CoveredParts(c, cover, tolerance)) {
            const Interval stretch{walked + part.first * step, walked + part.second * step};
            if (!covered.empty() && stretch.first <= covered.back().second + tolerance) covered.back().second = std::max(covered.back().second, stretch.second);
            else covered.push_back(stretch);
        }
        walked += step;
    }
    const double length = walked;
    // A stretch as short as the tolerance is where the line only starts from the end of
    // another one going the same way: not an overlap.
    std::erase_if(covered, [&](const Interval& stretch) {
        return stretch.second - stretch.first < 2 * tolerance && stretch.second - stretch.first < length - tolerance;
    });
    double from = 0;
    for (const Interval& stretch : covered) {
        if (stretch.first > from) out.push_back(Slice(line, from, stretch.first));
        from = std::max(from, stretch.second);
    }
    if (from < length) out.push_back(Slice(line, from, length));
    std::erase_if(out, [](const Polyline& piece) { return piece.size() < 2; });
    return out;
}

void Clean(std::vector<Polyline>& lines, Cover& cover, double tolerance) {
    std::vector<Polyline> kept;
    for (const Polyline& line : lines) {
        std::vector<Polyline> pieces = Uncovered(line, cover, tolerance);
        for (Polyline& piece : pieces) {
            if (Length(piece) <= tolerance) continue;   // a sliver between two overlaps, or an edge seen end-on
            cover.Add(piece, tolerance);
            kept.push_back(std::move(piece));
        }
    }
    lines = std::move(kept);
}

} // namespace

Polyline Slice(const Polyline& line, double from, double to) {
    Polyline out;
    double walked = 0;
    for (std::size_t i = 1; i < line.size() && walked < to; ++i) {
        const Vec2 a = line[i - 1], b = line[i];
        const double step = (b - a).Length();
        if (step <= 0) continue;
        if (walked + step > from) {
            if (out.empty()) out.push_back(a + (b - a) * std::clamp((from - walked) / step, 0.0, 1.0));
            out.push_back(a + (b - a) * std::clamp((to - walked) / step, 0.0, 1.0));
        }
        walked += step;
    }
    return out;
}

double TotalLength(const std::vector<Polyline>& lines) {
    double sum = 0;
    for (const Polyline& line : lines) sum += Length(line);
    return sum;
}

bool Covers(const std::vector<Polyline>& lines, Vec2 a, Vec2 b, double tolerance) {
    std::vector<Interval> parts;
    const Segment c{a, b};
    for (const Polyline& line : lines)
        for (std::size_t i = 1; i < line.size(); ++i) {
            Interval part;
            if (CoveredPart(c, {line[i - 1], line[i]}, tolerance, part)) parts.push_back(part);
        }
    std::sort(parts.begin(), parts.end());
    const double length = (b - a).Length();
    const double join = length > 0 ? tolerance / length : 1.0;
    double reached = 0;
    for (const Interval& p : parts) {
        if (p.first > reached + join) return false;
        reached = std::max(reached, p.second);
    }
    return reached >= 1 - join;
}

void RemoveOverlaps(ViewLines& lines, double tolerance) {
    const double big = std::numeric_limits<double>::max();
    Vec2 lo{big, big}, hi{-big, -big};
    for (const auto* set : {&lines.visible, &lines.hidden})
        for (const Polyline& line : *set)
            for (const Vec2 p : line) {
                lo = {std::min(lo.x, p.x), std::min(lo.y, p.y)};
                hi = {std::max(hi.x, p.x), std::max(hi.y, p.y)};
            }
    if (lo.x > hi.x) {
        lines = {};
        return;
    }
    const double cell = std::max({(hi.x - lo.x) / 64, (hi.y - lo.y) / 64, tolerance * 4});
    Cover cover(lo, cell);
    Clean(lines.visible, cover, tolerance);
    Clean(lines.hidden, cover, tolerance);   // the cover now holds the visible lines too

    lo = {big, big};
    hi = {-big, -big};
    for (const auto* set : {&lines.visible, &lines.hidden})
        for (const Polyline& line : *set)
            for (const Vec2 p : line) {
                lo = {std::min(lo.x, p.x), std::min(lo.y, p.y)};
                hi = {std::max(hi.x, p.x), std::max(hi.y, p.y)};
            }
    if (lo.x > hi.x) lo = hi = {};
    lines.min = lo;
    lines.max = hi;
}

} // namespace bz::drawing
