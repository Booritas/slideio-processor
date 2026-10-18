#include "core/detail/coverage.hpp"

#include <slideio/processor/core/error.hpp>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

// The conservative even-odd kernel. Everything happens in level pixel units,
// u = x / pixelW and v = y / pixelH, where level pixel (i, j) is the square
// [i, i+1] x [j, j+1] and its centre is (i+0.5, j+0.5) (plan ruling: absolute
// level mapping).
//
// Those units are taken relative to an integer level pixel (shiftX, shiftY)
// chosen from the POLYGON alone (see LocalAxis): u - shiftX is computed as
// (x - refX) / pixelW + fracX, with x - refX exact in int64_t and fracX the
// exact fractional level position of refX. Converting absolute coordinates to
// double first would collapse adjacent integers at and above 2^53. Because
// the reference depends on the polygon and the scale only, every grid
// computes bit-identical local values, and lattice lines stay integers. The
// grid box is shifted by the same integers, so the kernel below is unchanged
// and the "absolute" in what follows means "polygon-local".
//
// Step 2 (boundary): a pixel is marked when an edge passes through its OPEN
// interior. The cells are found row by row: the part of the edge inside the
// open strip j < v < j+1 projects onto an interval of u, and the cells whose
// open u-range meets that interval are the cells the edge passes through in
// that row. This visits exactly the cells an Amanatides-Woo walk visits, but
// every boundary decision is a single comparison of a value computed directly
// from the vertices, with no accumulated step error. An edge lying on a
// lattice line marks nothing, and an edge passing exactly through a lattice
// corner crosses diagonally without marking the two side cells, because the
// interval ends meet the corner's u exactly.
//
// Step 3 (interior): a pixel is marked when its centre is inside by even-odd,
// with the tie rule of Polygon::contains: an edge crosses the row's centre
// line v = y + 0.5 when (va <= vc) != (vb <= vc), and a centre is inside when
// it lies in [c(2k), c(2k+1)) of the sorted crossings.
//
// No epsilon is used anywhere, and no value that enters a decision depends on
// the grid origin: u and v round the same way for every tile, and every lattice
// comparison is against an absolute integer. Two grids with the same pixelW and
// pixelH sharing a seam therefore agree on every pixel by construction, at any
// such scale (4.0001 included). A BlockView grid is rounded first (see
// gridOfView), so this says nothing about how close it is to the true level
// grid. When
// pixelW and pixelH are powers of two the mapping is also exact.

namespace slideio::processor::detail {

namespace {

struct Edge {
    double ua, va, ub, vb;
};

struct Span {
    int64_t y, x0, x1;
};

struct Crossing {
    int64_t y;
    double u;
};

// An integer-valued double clamped into [lo, hi]; NaN maps to lo.
int64_t clampIndex(double d, int64_t lo, int64_t hi) {
    if (!(d > static_cast<double>(lo))) {
        return lo;
    }
    if (!(d < static_cast<double>(hi))) {
        return hi;
    }
    return static_cast<int64_t>(d);
}

bool isInteger(double d) { return std::floor(d) == d; }

// u on the edge's line at v; requires va != vb. The two-product form is exact
// at the endpoints and, for dyadic inputs, rounds to the exact value whenever
// that value is representable.
double uAt(const Edge& e, double v) {
    if (v == e.va) {
        return e.ua;
    }
    if (v == e.vb) {
        return e.ub;
    }
    return (e.ua * (e.vb - v) + e.ub * (v - e.va)) / (e.vb - e.va);
}

// The smallest integer x with x + 0.5 >= c, decided by an exact comparison.
double firstCentreAtOrAfter(double c) {
    const double f = std::floor(c);
    return f + 0.5 >= c ? f : f + 1.0;
}

class Kernel {
public:
    explicit Kernel(const PixelGrid& grid)
        : x0_(grid.originLevelX), x1_(grid.originLevelX + grid.width), y0_(grid.originLevelY),
          y1_(grid.originLevelY + grid.height), x0d_(static_cast<double>(x0_)),
          x1d_(static_cast<double>(x1_)), y0d_(static_cast<double>(y0_)),
          y1d_(static_cast<double>(y1_)) {}

    // Marks the cells of row j whose open u-range meets the interval [lo, hi], lo < hi.
    void markInterval(int64_t j, double lo, double hi) {
        const int64_t i0 = clampIndex(std::floor(lo), x0_, x1_);
        const int64_t i1 = clampIndex(std::ceil(hi), x0_, x1_);
        if (i0 < i1) {
            spans_.push_back(Span{j, i0, i1});
        }
    }

    // Marks the cell of row j containing u when u is strictly inside a cell.
    void markPoint(int64_t j, double u) {
        if (!isInteger(u) && u > x0d_ && u < x1d_) {
            const int64_t i = clampIndex(std::floor(u), x0_, x1_ - 1);
            spans_.push_back(Span{j, i, i + 1});
        }
    }

    void boundary(const Edge& e) {
        const double umin = std::min(e.ua, e.ub), umax = std::max(e.ua, e.ub);
        const double vmin = std::min(e.va, e.vb), vmax = std::max(e.va, e.vb);
        if (umax <= x0d_ || umin >= x1d_ || vmax <= y0d_ || vmin >= y1d_) {
            return; // cannot reach the open interior of any grid cell
        }
        if (e.va == e.vb) {
            // Horizontal: on a lattice line it marks nothing; inside a row it
            // marks every cell its open extent crosses.
            if (!isInteger(e.va) && umin < umax) {
                markInterval(clampIndex(std::floor(e.va), y0_, y1_ - 1), umin, umax);
            }
            return;
        }
        int64_t jlo = clampIndex(std::floor(vmin), y0_, y1_ - 1);
        int64_t jhi = clampIndex(std::ceil(vmax) - 1.0, y0_, y1_ - 1);
        if (e.ua == e.ub) {
            // Vertical: on a lattice line it marks nothing.
            if (isInteger(e.ua)) {
                return;
            }
            for (int64_t j = jlo; j <= jhi; ++j) {
                markPoint(j, e.ua);
            }
            return;
        }
        // Diagonal: limit the rows to where the edge is inside 0 <= u <= W, so
        // the work is bounded by the clipped edge. The clip is rounded, so the
        // row range is widened by one each way; each row is then decided exactly.
        double t0 = (x0d_ - e.ua) / (e.ub - e.ua), t1 = (x1d_ - e.ua) / (e.ub - e.ua);
        if (t0 > t1) {
            std::swap(t0, t1);
        }
        t0 = std::max(t0, 0.0);
        t1 = std::min(t1, 1.0);
        if (t0 <= t1) {
            const double c0 = e.va + t0 * (e.vb - e.va), c1 = e.va + t1 * (e.vb - e.va);
            jlo = std::max(jlo, clampIndex(std::floor(std::min(c0, c1)) - 1.0, y0_, y1_ - 1));
            jhi = std::min(jhi, clampIndex(std::ceil(std::max(c0, c1)), y0_, y1_ - 1));
        }
        for (int64_t j = jlo; j <= jhi; ++j) {
            const double s0 = std::max(vmin, static_cast<double>(j));
            const double s1 = std::min(vmax, static_cast<double>(j) + 1.0);
            if (!(s0 < s1)) {
                continue; // the edge only touches this row's boundary line
            }
            const double a = uAt(e, s0), b = uAt(e, s1);
            if (a != b) {
                markInterval(j, std::min(a, b), std::max(a, b));
            } else {
                markPoint(j, a); // a sub-ulp sliver: treat as vertical
            }
        }
    }

    void crossings(const Edge& e) {
        if (e.va == e.vb) {
            return;
        }
        const double vmin = std::min(e.va, e.vb), vmax = std::max(e.va, e.vb);
        if (vmax <= y0d_ || vmin >= y1d_) {
            return;
        }
        // Rows with vmin <= y + 0.5 < vmax, widened by one each way and then
        // decided by the exact predicate.
        const int64_t ylo = clampIndex(std::ceil(vmin - 0.5) - 1.0, y0_, y1_ - 1);
        const int64_t yhi = clampIndex(std::ceil(vmax - 0.5), y0_, y1_ - 1);
        for (int64_t y = ylo; y <= yhi; ++y) {
            const double vc = static_cast<double>(y) + 0.5;
            if ((e.va <= vc) != (e.vb <= vc)) {
                crossings_.push_back(Crossing{y, uAt(e, vc)});
            }
        }
    }

    void interior() {
        // Callers guarantee scale >= 1e-6, so every u is finite; a NaN here would
        // break the strict weak ordering std::sort needs.
        assert(std::none_of(crossings_.begin(), crossings_.end(),
                            [](const Crossing& c) { return std::isnan(c.u); }));
        std::sort(crossings_.begin(), crossings_.end(), [](const Crossing& a, const Crossing& b) {
            return a.y != b.y ? a.y < b.y : a.u < b.u;
        });
        size_t k = 0;
        while (k < crossings_.size()) {
            size_t end = k;
            while (end < crossings_.size() && crossings_[end].y == crossings_[k].y) {
                ++end;
            }
            // Closed rings give an even count per row; a stray last one is ignored.
            for (size_t c = k; c + 1 < end; c += 2) {
                const int64_t x0 = clampIndex(firstCentreAtOrAfter(crossings_[c].u), x0_, x1_);
                const int64_t x1 = clampIndex(firstCentreAtOrAfter(crossings_[c + 1].u), x0_, x1_);
                if (x0 < x1) {
                    spans_.push_back(Span{crossings_[k].y, x0, x1});
                }
            }
            k = end;
        }
    }

    void emit(const std::function<void(int64_t, int64_t, int64_t)>& row) {
        std::sort(spans_.begin(), spans_.end(), [](const Span& a, const Span& b) {
            return a.y != b.y ? a.y < b.y : a.x0 < b.x0;
        });
        std::vector<Span> merged;
        merged.reserve(spans_.size());
        for (const Span& s : spans_) {
            if (!merged.empty() && merged.back().y == s.y && s.x0 <= merged.back().x1) {
                merged.back().x1 = std::max(merged.back().x1, s.x1);
            } else {
                merged.push_back(s);
            }
        }
        for (const Span& s : merged) {
            row(s.y - y0_, s.x0 - x0_, s.x1 - x0_); // absolute -> grid-relative
        }
    }

private:
    int64_t x0_, x1_, y0_, y1_; // the grid box in absolute level pixels, half-open
    double x0d_, x1d_, y0d_, y1d_;
    std::vector<Span> spans_;
    std::vector<Crossing> crossings_;
};

// The error-free sum a + b = s + t (Knuth's TwoSum).
void twoSum(double a, double b, double& s, double& t) {
    s = a + b;
    const double bb = s - a;
    t = (a - (s - bb)) + (b - bb);
}

// One axis of the polygon-local frame: level position of x is
// shift + (x - ref) / pixel + frac, with shift an integer and frac in [0, 1).
struct LocalAxis {
    int64_t ref = 0;
    int64_t shift = 0;
    double frac = 0.0;
    double pixel = 1.0;

    double map(int64_t x) const { return static_cast<double>(x - ref) / pixel + frac; }
};

// Splits ref / pixel into shift + frac exactly enough that frac is accurate to
// an ulp of 1. Falls back to the absolute mapping (ref 0) when ref / pixel is
// beyond 2^62 or when the grid box shifted by it would overflow.
LocalAxis localAxis(int64_t ref, double pixel, int64_t gridLo, int64_t gridHi) {
    LocalAxis a;
    a.pixel = pixel;
    const double qd = std::floor(static_cast<double>(ref) / pixel);
    constexpr double kLimit = 4611686018427387904.0; // 2^62
    if (!(std::fabs(qd) < kLimit)) {
        return a;
    }
    // r = ref - q * pixel, exactly enough: ref = H + L with both parts exact
    // doubles, q * pixel = p + e exactly (fma), and the large terms cancel.
    const int64_t hi = static_cast<int64_t>(static_cast<uint64_t>(ref) & ~uint64_t{0xFFFFFFFF});
    const double hd = static_cast<double>(hi), ld = static_cast<double>(ref - hi);
    const double p = qd * pixel, e = std::fma(qd, pixel, -p);
    double s1, t1;
    twoSum(hd, -p, s1, t1);
    double r = ((s1 + ld) + t1) - e;
    int64_t q = static_cast<int64_t>(qd);
    // The rounded quotient can be off by many units for large ref; fold that back.
    const double k = std::floor(r / pixel);
    if (k != 0.0 && std::fabs(k) < kLimit) {
        q += static_cast<int64_t>(k);
        r = std::fma(-k, pixel, r);
    }
    if (r < 0.0) {
        q -= 1;
        r += pixel;
    } else if (r >= pixel) {
        q += 1;
        r -= pixel;
    }
    const double f = r / pixel;
    // Shifting the grid box must not overflow.
    constexpr int64_t kMin = std::numeric_limits<int64_t>::min();
    constexpr int64_t kMax = std::numeric_limits<int64_t>::max();
    const auto subFits = [&](int64_t v) { return q >= 0 ? v >= kMin + q : v <= kMax + q; };
    if (!(f >= 0.0) || !subFits(gridLo) || !subFits(gridHi)) {
        return a;
    }
    a.ref = ref;
    a.shift = q;
    a.frac = f < 1.0 ? f : std::nextafter(1.0, 0.0);
    return a;
}

} // namespace

void forEachCoveredSpan(const Polygon& polygon, const PixelGrid& grid,
                        const std::function<void(int64_t y, int64_t x0, int64_t x1)>& row) {
    if (polygon.isEmpty() || grid.width <= 0 || grid.height <= 0) {
        return;
    }
    // The polygon-local frame: its reference is the polygon's lowest corner, so
    // every vertex difference x - ref is exact and non-negative (addRing keeps
    // the extent within int64_t).
    const Rect64 box = polygon.boundingBox();
    const LocalAxis ax = localAxis(box.x, grid.pixelW, grid.originLevelX, grid.originLevelX + grid.width);
    const LocalAxis ay = localAxis(box.y, grid.pixelH, grid.originLevelY, grid.originLevelY + grid.height);
    PixelGrid local = grid;
    local.originLevelX = grid.originLevelX - ax.shift;
    local.originLevelY = grid.originLevelY - ay.shift;
    Kernel kernel(local);
    std::vector<Point64> points;
    for (int32_t r = 0; r < polygon.ringCount(); ++r) {
        const int64_t n = polygon.ringPointCount(r);
        if (n < 2) {
            continue;
        }
        points.resize(static_cast<size_t>(n));
        polygon.ringPoints(r, points.data(), n);
        for (int64_t k = 0; k < n; ++k) {
            const Point64& a = points[static_cast<size_t>(k)];
            const Point64& b = points[static_cast<size_t>(k + 1 == n ? 0 : k + 1)];
            // Polygon-local level units: the rounding depends on the vertex, the
            // polygon and the scale only, never on which grid asked.
            const Edge e{ax.map(a.x), ay.map(a.y), ax.map(b.x), ay.map(b.y)};
            kernel.boundary(e);
            kernel.crossings(e);
        }
    }
    kernel.interior();
    kernel.emit(row);
}

PixelGrid gridOfView(const BlockView& view) {
    if (view.isNull()) {
        throw InvalidArgumentError("rasteriseInto: view is null");
    }
    const int64_t width = view.width(), height = view.height();
    const Rect64 rect = view.slideRect();
    const double sx = static_cast<double>(rect.width) / static_cast<double>(width);
    const double sy = static_cast<double>(rect.height) / static_cast<double>(height);
    if (!std::isfinite(sx) || !std::isfinite(sy) || !(sx >= kMinLevelScale) || !(sy >= kMinLevelScale)) {
        throw InvalidArgumentError("rasteriseInto: view scale must be finite and at least 1e-6");
    }
    const double ox = std::round(static_cast<double>(rect.x) / sx);
    const double oy = std::round(static_cast<double>(rect.y) / sy);
    constexpr double kOriginLimit = 4611686018427387904.0; // 2^62
    if (!(std::fabs(ox) < kOriginLimit) || !(std::fabs(oy) < kOriginLimit)) {
        throw InvalidArgumentError("rasteriseInto: view origin does not fit");
    }
    const int64_t originX = static_cast<int64_t>(ox), originY = static_cast<int64_t>(oy);
    if (originX > std::numeric_limits<int64_t>::max() - width ||
        originY > std::numeric_limits<int64_t>::max() - height) {
        throw InvalidArgumentError("rasteriseInto: view origin does not fit");
    }
    return PixelGrid{originX, originY, sx, sy, width, height};
}

} // namespace slideio::processor::detail
