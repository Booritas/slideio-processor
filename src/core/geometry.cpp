#include <slideio/processor/core/geometry.hpp>

#include <slideio/processor/core/error.hpp>

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace slideio::processor {

bool operator==(Point64 a, Point64 b) noexcept { return a.x == b.x && a.y == b.y; }

bool operator==(const Rect64& a, const Rect64& b) noexcept {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

int64_t Rect64::right() const noexcept { return x + width; }

int64_t Rect64::bottom() const noexcept { return y + height; }

int64_t Rect64::area() const noexcept { return width * height; }

bool Rect64::isEmpty() const noexcept { return width <= 0 || height <= 0; }

bool Rect64::contains(Point64 p) const noexcept {
    return p.x >= x && p.x < right() && p.y >= y && p.y < bottom();
}

bool Rect64::intersects(const Rect64& other) const noexcept {
    if (isEmpty() || other.isEmpty()) {
        return false;
    }
    return x < other.right() && other.x < right() && y < other.bottom() && other.y < bottom();
}

Rect64 Rect64::intersected(const Rect64& other) const noexcept {
    if (!intersects(other)) {
        return Rect64{0, 0, 0, 0};
    }
    const int64_t left = std::max(x, other.x);
    const int64_t top = std::max(y, other.y);
    const int64_t r = std::min(right(), other.right());
    const int64_t b = std::min(bottom(), other.bottom());
    return Rect64{left, top, r - left, b - top};
}

Rect64 Rect64::united(const Rect64& other) const noexcept {
    if (isEmpty()) {
        return other.isEmpty() ? Rect64{0, 0, 0, 0} : other;
    }
    if (other.isEmpty()) {
        return *this;
    }
    const int64_t left = std::min(x, other.x);
    const int64_t top = std::min(y, other.y);
    const int64_t r = std::max(right(), other.right());
    const int64_t b = std::max(bottom(), other.bottom());
    return Rect64{left, top, r - left, b - top};
}

// Storage: every vertex of every ring in one array, plus where each ring starts.
class Polygon::Impl {
public:
    std::vector<Point64> points;
    std::vector<int64_t> starts; // index in points of each ring's first vertex
    std::vector<char> holes;     // one flag per ring

    int64_t ringBegin(size_t ring) const { return starts[ring]; }
    int64_t ringEnd(size_t ring) const {
        return ring + 1 < starts.size() ? starts[ring + 1] : static_cast<int64_t>(points.size());
    }
};

namespace {

// Shoelace sums of one ring, taken relative to its first vertex so that large
// coordinates cost no precision.
struct RingSums {
    double twiceArea = 0;  // signed
    double cxSum = 0;      // sum of (xi + xj) * cross, relative coordinates
    double cySum = 0;
    double length = 0;
};

RingSums sumRing(const Point64* p, int64_t n) {
    RingSums s;
    for (int64_t i = 0; i < n; ++i) {
        const Point64& a = p[i];
        const Point64& b = p[i + 1 == n ? 0 : i + 1];
        const double ax = static_cast<double>(a.x) - static_cast<double>(p[0].x);
        const double ay = static_cast<double>(a.y) - static_cast<double>(p[0].y);
        const double bx = static_cast<double>(b.x) - static_cast<double>(p[0].x);
        const double by = static_cast<double>(b.y) - static_cast<double>(p[0].y);
        const double cross = ax * by - bx * ay;
        s.twiceArea += cross;
        s.cxSum += (ax + bx) * cross;
        s.cySum += (ay + by) * cross;
        s.length += std::hypot(bx - ax, by - ay);
    }
    return s;
}

} // namespace

Polygon::Polygon() : impl_(std::make_unique<Impl>()) {}

Polygon::Polygon(const Polygon& other) : impl_(other.impl_ ? std::make_unique<Impl>(*other.impl_) : nullptr) {}

Polygon::Polygon(Polygon&& other) noexcept : impl_(std::move(other.impl_)) {}

Polygon& Polygon::operator=(const Polygon& other) {
    if (this != &other) {
        impl_ = other.impl_ ? std::make_unique<Impl>(*other.impl_) : nullptr;
    }
    return *this;
}

Polygon& Polygon::operator=(Polygon&& other) noexcept {
    if (this != &other) {
        impl_ = std::move(other.impl_);
    }
    return *this;
}

Polygon::~Polygon() = default;

int32_t Polygon::ringCount() const noexcept {
    return impl_ ? static_cast<int32_t>(impl_->starts.size()) : 0;
}

bool Polygon::ringIsHole(int32_t ring) const {
    if (ring < 0 || ring >= ringCount()) {
        throw InvalidArgumentError("ring index out of range");
    }
    return impl_->holes[static_cast<size_t>(ring)] != 0;
}

int64_t Polygon::ringPointCount(int32_t ring) const {
    if (ring < 0 || ring >= ringCount()) {
        throw InvalidArgumentError("ring index out of range");
    }
    const size_t r = static_cast<size_t>(ring);
    return impl_->ringEnd(r) - impl_->ringBegin(r);
}

int64_t Polygon::ringPoints(int32_t ring, Point64* buffer, int64_t capacity) const {
    const int64_t n = ringPointCount(ring);
    if (buffer != nullptr && capacity > 0) {
        const Point64* first = impl_->points.data() + impl_->ringBegin(static_cast<size_t>(ring));
        std::copy(first, first + std::min(n, capacity), buffer);
    }
    return n;
}

void Polygon::addRing(const Point64* points, int64_t count, bool isHole) {
    if (points == nullptr) {
        throw InvalidArgumentError("ring points must not be null");
    }
    int64_t n = count;
    if (n >= 1 && points[n - 1] == points[0]) {
        --n;
    }
    if (n < 3) {
        throw InvalidArgumentError("a ring needs at least 3 distinct points");
    }
    if (isHole && isEmpty()) {
        throw InvalidArgumentError("the first ring must be an outer ring");
    }
    if (!impl_) {
        impl_ = std::make_unique<Impl>();
    }
    Impl& d = *impl_;
    const size_t oldPoints = d.points.size();
    const size_t oldRings = d.starts.size();
    try {
        d.points.insert(d.points.end(), points, points + n);
        d.starts.push_back(static_cast<int64_t>(oldPoints));
        d.holes.push_back(isHole ? 1 : 0);
    } catch (...) {
        d.points.resize(oldPoints);
        d.starts.resize(oldRings);
        d.holes.resize(oldRings);
        throw;
    }
}

void Polygon::clear() noexcept {
    if (impl_) {
        impl_->points.clear();
        impl_->starts.clear();
        impl_->holes.clear();
    }
}

bool Polygon::isEmpty() const noexcept { return ringCount() == 0; }

Rect64 Polygon::boundingBox() const noexcept {
    if (isEmpty()) {
        return Rect64{0, 0, 0, 0};
    }
    Point64 lo = impl_->points[0];
    Point64 hi = lo;
    for (const Point64& p : impl_->points) {
        lo.x = std::min(lo.x, p.x);
        lo.y = std::min(lo.y, p.y);
        hi.x = std::max(hi.x, p.x);
        hi.y = std::max(hi.y, p.y);
    }
    return Rect64{lo.x, lo.y, hi.x - lo.x, hi.y - lo.y};
}

Point64 Polygon::anchor() const noexcept {
    if (isEmpty()) {
        return Point64{0, 0};
    }
    Point64 best = impl_->points[0];
    for (const Point64& p : impl_->points) {
        if (p.y < best.y || (p.y == best.y && p.x < best.x)) {
            best = p;
        }
    }
    return best;
}

double Polygon::area() const noexcept {
    double total = 0;
    for (int32_t r = 0; r < ringCount(); ++r) {
        const size_t i = static_cast<size_t>(r);
        const RingSums s = sumRing(impl_->points.data() + impl_->ringBegin(i),
                                   impl_->ringEnd(i) - impl_->ringBegin(i));
        const double a = std::fabs(s.twiceArea) / 2;
        total += impl_->holes[i] ? -a : a;
    }
    return total;
}

double Polygon::perimeter() const noexcept {
    double total = 0;
    for (int32_t r = 0; r < ringCount(); ++r) {
        const size_t i = static_cast<size_t>(r);
        total += sumRing(impl_->points.data() + impl_->ringBegin(i),
                         impl_->ringEnd(i) - impl_->ringBegin(i)).length;
    }
    return total;
}

Point64 Polygon::centroid() const noexcept {
    if (isEmpty()) {
        return Point64{0, 0};
    }
    // Weighted sum in coordinates relative to ring 0's first vertex.
    const Point64 ref = impl_->points[0];
    double weight = 0;
    double wx = 0;
    double wy = 0;
    for (int32_t r = 0; r < ringCount(); ++r) {
        const size_t i = static_cast<size_t>(r);
        const Point64* p = impl_->points.data() + impl_->ringBegin(i);
        const RingSums s = sumRing(p, impl_->ringEnd(i) - impl_->ringBegin(i));
        if (s.twiceArea == 0) {
            continue;
        }
        const double a = std::fabs(s.twiceArea) / 2;
        const double w = impl_->holes[i] ? -a : a;
        const double cx = static_cast<double>(p[0].x) - static_cast<double>(ref.x) + s.cxSum / (3 * s.twiceArea);
        const double cy = static_cast<double>(p[0].y) - static_cast<double>(ref.y) + s.cySum / (3 * s.twiceArea);
        weight += w;
        wx += w * cx;
        wy += w * cy;
    }
    double mx;
    double my;
    if (weight != 0) {
        mx = wx / weight;
        my = wy / weight;
    } else {
        // Zero area: the mean of ring 0's vertices.
        const int64_t n = impl_->ringEnd(0);
        double sx = 0;
        double sy = 0;
        for (int64_t k = 0; k < n; ++k) {
            sx += static_cast<double>(impl_->points[static_cast<size_t>(k)].x) - static_cast<double>(ref.x);
            sy += static_cast<double>(impl_->points[static_cast<size_t>(k)].y) - static_cast<double>(ref.y);
        }
        mx = sx / static_cast<double>(n);
        my = sy / static_cast<double>(n);
    }
    return Point64{static_cast<int64_t>(std::llround(static_cast<double>(ref.x) + mx)),
                   static_cast<int64_t>(std::llround(static_cast<double>(ref.y) + my))};
}

bool Polygon::contains(Point64 p) const noexcept {
    if (isEmpty()) {
        return false;
    }
    // Even-odd test at the centre of pixel p; coordinates relative to p.
    const double yc = 0.5;
    const double xc = 0.5;
    bool inside = false;
    for (int32_t r = 0; r < ringCount(); ++r) {
        const size_t i = static_cast<size_t>(r);
        const Point64* v = impl_->points.data() + impl_->ringBegin(i);
        const int64_t n = impl_->ringEnd(i) - impl_->ringBegin(i);
        for (int64_t k = 0; k < n; ++k) {
            const Point64& a = v[k];
            const Point64& b = v[k + 1 == n ? 0 : k + 1];
            const double ay = static_cast<double>(a.y) - static_cast<double>(p.y);
            const double by = static_cast<double>(b.y) - static_cast<double>(p.y);
            if ((ay <= yc) == (by <= yc)) {
                continue;
            }
            const double ax = static_cast<double>(a.x) - static_cast<double>(p.x);
            const double bx = static_cast<double>(b.x) - static_cast<double>(p.x);
            const double dy = by - ay;
            const double lhs = (xc - ax) * dy;
            const double rhs = (yc - ay) * (bx - ax);
            // The edge crosses the ray's row to the right of the point.
            if (dy > 0 ? lhs < rhs : lhs > rhs) {
                inside = !inside;
            }
        }
    }
    return inside;
}

} // namespace slideio::processor
