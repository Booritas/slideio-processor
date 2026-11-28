// Requirements: UR-SEG-8, UR-QA-2, UR-QA-3
#include <algorithm/support/flood_model.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <stdexcept>

// Written from SDS 06 "Watershed, exactly" and the design
// 2026-11-28-watershed-unreached-foreground "Behaviour" / "Tile invariance: the
// gate". It shares no code with src/algorithm/segment/watershed.cpp and does not
// mirror its structure: no priority queue, no halo, no tiles. Every step is a
// scan of the whole image, which is O(n^2) and fine at harness sizes.

namespace slideio::processor::testing_support {

namespace {

struct Grid {
    const std::vector<double>& values;
    int64_t width;
    int64_t height;

    int64_t x(int64_t i) const { return i % width; }
    int64_t y(int64_t i) const { return i / width; }
    double value(int64_t i) const { return values[static_cast<size_t>(i)]; }

    // SDS 06 "Foreground": neither 0 nor NaN (-0 is 0; infinities are foreground).
    bool foreground(int64_t i) const {
        const double v = value(i);
        return v != 0.0 && !std::isnan(v);
    }

    // Precedence among foreground pixels: greater value first, then level
    // coordinates, row then column, which is the row-major index.
    bool precedes(int64_t a, int64_t b) const {
        return value(a) > value(b) || (value(a) == value(b) && a < b);
    }

    int64_t chebyshev(int64_t a, int64_t b) const {
        return std::max(std::abs(x(a) - x(b)), std::abs(y(a) - y(b)));
    }
};

// SDS 06 "Markers", LocalMaxima: a foreground pixel that comes before every
// other foreground pixel within the marker distance (Chebyshev), and, with a
// floor, is not below it.
//
// At the level edge the window is clipped to the level: what lies beyond the
// level is background. The SDS says a pixel "whose window reaches beyond the
// halo'd context is not decided there", and the framework reads pixels beyond
// the level as zero (the harness's applyDirect documents this "as pixels beyond
// the level do in a run"), so the halo'd context of a tile at the level edge
// extends past the level with background, and a window reaching past the level
// is decided. A model whose one context is exactly the level reads the same
// thing by treating out-of-level pixels as background.
// Taken from the code, after the model was written, because the SDS section does
// not say whether a context stops at the level: OperationContext::haloRectAtLevel()
// (operation.hpp) is "not clipped to the level ... those pixels read as zero", and
// watershed.cpp's detectMaxima decides only windows inside that context. So the
// operation never sees a context equal to the level, and a pixel near the level
// edge is decided against zeros, as here.
bool isLocalMaximum(const Grid& g, int64_t i, const ModelParameters& p) {
    if (!g.foreground(i)) {
        return false;
    }
    if (p.floor && g.value(i) < *p.floor) {
        return false;
    }
    const int64_t cx = g.x(i);
    const int64_t cy = g.y(i);
    for (int64_t v = std::max<int64_t>(0, cy - p.markerDistance);
         v <= std::min(g.height - 1, cy + p.markerDistance); ++v) {
        for (int64_t u = std::max<int64_t>(0, cx - p.markerDistance);
             u <= std::min(g.width - 1, cx + p.markerDistance); ++u) {
            const int64_t j = v * g.width + u;
            if (j != i && g.foreground(j) && !g.precedes(i, j)) {
                return false;
            }
        }
    }
    return true;
}

} // namespace

ModelResult modelWatershed(const std::vector<double>& values, int64_t width, int64_t height,
                           const ModelParameters& p) {
    if (width <= 0 || height <= 0 || values.size() != static_cast<size_t>(width * height)) {
        throw std::invalid_argument("modelWatershed: values do not match width x height");
    }
    const bool auxiliary = !p.anchors.empty();
    if (auxiliary && p.anchors.size() != values.size()) {
        throw std::invalid_argument("modelWatershed: anchors do not match width x height");
    }
    const Grid g{values, width, height};
    const int64_t n = width * height;

    std::vector<bool> flooded(static_cast<size_t>(n), false);
    std::vector<int64_t> seed(static_cast<size_t>(n), -1); // the seed pixel a flooded pixel was reached from
    ModelResult result;
    result.key.assign(static_cast<size_t>(n), kModelBackground);

    // Markers: each marker pixel is flooded from the start and is its own seed.
    // With anchors every foreground pixel whose anchor is not -1 is one, and one
    // basin is one anchor value; the floor applies to LocalMaxima only (the
    // operation refuses it with AuxiliaryRaster).
    for (int64_t i = 0; i < n; ++i) {
        const bool marker = auxiliary ? g.foreground(i) && p.anchors[static_cast<size_t>(i)] != -1
                                      : isLocalMaximum(g, i, p);
        if (marker) {
            flooded[static_cast<size_t>(i)] = true;
            seed[static_cast<size_t>(i)] = i;
            result.key[static_cast<size_t>(i)] =
                auxiliary ? p.anchors[static_cast<size_t>(i)] : (g.y(i) << 31) | g.x(i);
        }
    }

    // A flooded side neighbour of @p i that may claim it: under DropBounded only
    // one whose seed lies within the spacing of i.
    const auto claims = [&](int64_t i, int64_t neighbour) {
        if (!flooded[static_cast<size_t>(neighbour)]) {
            return false;
        }
        return p.rule != ModelRule::DropBounded || g.chebyshev(i, seed[static_cast<size_t>(neighbour)]) <= p.spacing;
    };
    // The highest-precedence neighbour of @p i that may claim it, or -1.
    const auto claimant = [&](int64_t i) {
        const int64_t x = g.x(i);
        const int64_t y = g.y(i);
        int64_t best = -1;
        const int64_t candidates[4] = {y > 0 ? i - width : -1, x > 0 ? i - 1 : -1, x + 1 < width ? i + 1 : -1,
                                       y + 1 < height ? i + width : -1};
        for (const int64_t c : candidates) {
            if (c >= 0 && claims(i, c) && (best < 0 || g.precedes(c, best))) {
                best = c;
            }
        }
        return best;
    };

    // The flood: repeatedly the highest-precedence unflooded foreground pixel
    // with a neighbour that may claim it joins that neighbour's basin and seed.
    // Under DropBounded a pixel no neighbour may claim stays unflooded for now
    // (another neighbour flooded later from a nearer seed may still claim it),
    // and while unflooded it propagates nothing.
    for (;;) {
        int64_t next = -1;
        int64_t from = -1;
        for (int64_t i = 0; i < n; ++i) {
            if (!g.foreground(i) || flooded[static_cast<size_t>(i)]) {
                continue;
            }
            if (next >= 0 && !g.precedes(i, next)) {
                continue;
            }
            const int64_t c = claimant(i);
            if (c >= 0) {
                next = i;
                from = c;
            }
        }
        if (next < 0) {
            break;
        }
        flooded[static_cast<size_t>(next)] = true;
        seed[static_cast<size_t>(next)] = seed[static_cast<size_t>(from)];
        result.key[static_cast<size_t>(next)] = result.key[static_cast<size_t>(from)];
    }

    // The reach check: every foreground pixel flooded from a seed within the
    // spacing. Under Fail anything else refuses the run (an unreached pixel is
    // reported as kModelDropped, one reached from too far keeps its key); under
    // both Drop rules such pixels are cleared.
    for (int64_t i = 0; i < n; ++i) {
        if (!g.foreground(i)) {
            continue;
        }
        const bool reached = flooded[static_cast<size_t>(i)];
        const bool within = reached && g.chebyshev(i, seed[static_cast<size_t>(i)]) <= p.spacing;
        if (within) {
            continue;
        }
        if (p.rule == ModelRule::Fail) {
            result.refused = true;
            if (!reached) {
                result.key[static_cast<size_t>(i)] = kModelDropped;
            }
        } else {
            result.key[static_cast<size_t>(i)] = kModelDropped;
        }
    }
    return result;
}

} // namespace slideio::processor::testing_support
