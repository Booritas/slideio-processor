#include "core/detail/coverage.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <vector>

// The conservative even-odd kernel. Everything happens in ABSOLUTE level pixel
// units, u = x / pixelW and v = y / pixelH, where level pixel (i, j) is the
// square [i, i+1] x [j, j+1] and its centre is (i+0.5, j+0.5). The grid's
// integer origin is used only to clip and, at the very end, to turn absolute
// pixel indices into grid-relative ones (plan ruling: absolute level mapping).
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
// comparison is against an absolute integer. Two grids sharing a seam therefore
// agree on every pixel by construction, at any scale (4.0001 included). When
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

} // namespace

void forEachCoveredSpan(const Polygon& polygon, const PixelGrid& grid,
                        const std::function<void(int64_t y, int64_t x0, int64_t x1)>& row) {
    if (polygon.isEmpty() || grid.width <= 0 || grid.height <= 0) {
        return;
    }
    Kernel kernel(grid);
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
            // Absolute level units: the rounding depends on the vertex and the
            // scale only, never on which grid asked.
            const Edge e{static_cast<double>(a.x) / grid.pixelW, static_cast<double>(a.y) / grid.pixelH,
                         static_cast<double>(b.x) / grid.pixelW, static_cast<double>(b.y) / grid.pixelH};
            kernel.boundary(e);
            kernel.crossings(e);
        }
    }
    kernel.interior();
    kernel.emit(row);
}

} // namespace slideio::processor::detail
