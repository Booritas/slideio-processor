// Requirements: UR-SEG-8, UR-QA-2, UR-QA-3
//
// The tile invariance gate for segment.watershed's Drop mode (design
// 2026-11-28-watershed-unreached-foreground, "Tile invariance: the gate"). Each
// input is run three ways and compared:
//   (b) the operation over one tile against the whole-image reference model
//       (support/flood_model, rule DropBounded): the same partition and the same
//       dropped pixels; and Fail's refusal against the model's;
//   (a) the operation tiled at several sizes and worker counts against the
//       operation over one tile: the same objects and the same dropped pixels;
//   (c) where Fail runs at every tile size, Drop against Fail: nothing dropped
//       and the same objects.
// Fail passing at some tile sizes and refusing at others is a Fail-mode finding,
// logged and not failed.
//
// This is a measuring instrument: a mismatch is a result about the operation,
// recorded with its seed and parameters, not something to tune away here.
//
// The executable is labelled `slow` and kept out of the default suite's time
// budget. Inputs run concurrently, one input per thread, each run's pipeline
// with its own workers; every Pipeline, scene and anchor source is the input's
// own.

#include <slideio/processor/algorithm/segment/segment.hpp>
#include <slideio/processor/core/data_type.hpp>
#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/measurement.hpp>
#include <slideio/processor/core/object_record.hpp>
#include <slideio/processor/pipeline/pipeline_spec.hpp>

#include <algorithm/support/flood_model.hpp>
#include <algorithm/support/harness.hpp>
#include <pipeline/test_support/synthetic_scene.hpp>

#include <slideio/slideio/scene.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace slideio::processor;
namespace ts = slideio::processor::testing_support;

namespace {

// Seeds 0 .. kRandomInputs - 1.
constexpr int32_t kRandomInputs = 30000;
// A tile no input reaches the side of: the single-tile reference.
constexpr int32_t kSingleTile = 512;
// The model rule the operation's Drop mode is compared with: bounded claims
// (design step 4a), since the plain rule was found tile-dependent.
constexpr ts::ModelRule kDropRule = ts::ModelRule::DropBounded;
constexpr const char* kDropRuleName = "DropBounded";

// ---------------------------------------------------------------------------
// Inputs

struct TiledRun {
    int32_t tile;
    int32_t workers;
};

struct Input {
    std::string name;
    std::string generator;
    int64_t width = 0;
    int64_t height = 0;
    bool byteSamples = false;
    std::vector<double> values; // row-major, each exactly representable in the sample type
    int64_t spacing = 1;
    int64_t markerDistance = 1;
    std::optional<double> floor;
    std::vector<int64_t> anchors; // empty: LocalMaxima; else per pixel, -1 = none
    bool auxiliary = false;       // AuxiliaryRaster markers (anchors is cleared once the input has run)
    int64_t feature = 0;          // the generators' largest feature size; 0: up to half the level
    std::vector<TiledRun> runs;   // the tiled Drop runs
};

bool foreground(double v) { return v != 0.0 && !std::isnan(v); }

// The sample as the operation reads it: Float32 rounds, Byte saturates.
double asSample(double v, bool byteSamples) {
    if (byteSamples) {
        return std::min(255.0, std::max(0.0, std::floor(v)));
    }
    return static_cast<double>(static_cast<float>(v));
}

// The halo of segment.watershed in Drop mode at spacing s: 4 s + 1.
int64_t dropHalo(int64_t spacing) { return 4 * spacing + 1; }

// Whether some tile of a @p tile-px grid over a width x height level, from its
// origin, has a halo'd context (inflated by @p halo, clipped to the level)
// strictly smaller than the level in at least one dimension: a run in which
// some tile cannot see the whole image, so tile invariance is not trivial.
bool someContextSmallerThanLevel(int64_t width, int64_t height, int64_t tile, int64_t halo) {
    const auto partial = [tile, halo](int64_t length) {
        for (int64_t start = 0; start < length; start += tile) {
            const int64_t lo = std::max<int64_t>(0, start - halo);
            const int64_t hi = std::min(length, start + tile + halo);
            if (hi - lo < length) {
                return true;
            }
        }
        return false;
    };
    return partial(width) || partial(height);
}

// The tile sizes the brief names, by role, for spacing s: 1, 3, 7, halo / 2,
// halo and 2 halo + 1, where halo = 4 s + 1, Drop's halo.
std::vector<int32_t> roleTiles(int64_t spacing) {
    const auto halo = static_cast<int32_t>(dropHalo(spacing));
    return {1, 3, 7, halo / 2, halo, 2 * halo + 1};
}
const char* const kRoleNames[] = {"1", "3", "7", "halo/2", "halo", "2*halo+1"};

int64_t uniform(std::mt19937_64& rng, int64_t lo, int64_t hi) {
    return std::uniform_int_distribution<int64_t>(lo, hi)(rng);
}

bool chance(std::mt19937_64& rng, double p) { return std::uniform_real_distribution<double>(0.0, 1.0)(rng) < p; }

// A palette of k distinct sample values: integers for Byte, quarters for Float32.
std::vector<double> palette(std::mt19937_64& rng, bool byteSamples, int64_t k) {
    std::set<double> values;
    while (static_cast<int64_t>(values.size()) < k) {
        values.insert(byteSamples ? static_cast<double>(uniform(rng, 1, 12))
                                  : static_cast<double>(uniform(rng, 1, 40)) * 0.25);
    }
    return std::vector<double>(values.begin(), values.end());
}

// Random rectangles painted over a background (zero, or a palette value):
// few distinct values, plateaus, ties everywhere.
void paintRectangles(std::mt19937_64& rng, Input& in) {
    const std::vector<double> colours = palette(rng, in.byteSamples, uniform(rng, 2, 5));
    const auto pick = [&] { return colours[static_cast<size_t>(uniform(rng, 0, static_cast<int64_t>(colours.size()) - 1))]; };
    const bool filled = in.feature == 0 && chance(rng, 0.3);
    in.values.assign(static_cast<size_t>(in.width * in.height), filled ? pick() : 0.0);
    const int64_t count = uniform(rng, 3, 14);
    const int64_t maxWidth = in.feature != 0 ? std::min(in.feature, in.width / 2) : in.width / 2;
    const int64_t maxHeight = in.feature != 0 ? std::min(in.feature, in.height / 2) : in.height / 2;
    for (int64_t r = 0; r < count; ++r) {
        const int64_t w = uniform(rng, 1, std::max<int64_t>(1, maxWidth));
        const int64_t h = uniform(rng, 1, std::max<int64_t>(1, maxHeight));
        const int64_t x0 = uniform(rng, -w / 2, in.width - 1);
        const int64_t y0 = uniform(rng, -h / 2, in.height - 1);
        const double v = chance(rng, 0.2) ? 0.0 : pick();
        for (int64_t y = std::max<int64_t>(0, y0); y < std::min(in.height, y0 + h); ++y) {
            for (int64_t x = std::max<int64_t>(0, x0); x < std::min(in.width, x0 + w); ++x) {
                in.values[static_cast<size_t>(y * in.width + x)] = v;
            }
        }
    }
}

// Blocks of c x c pixels, each a palette value or background.
void paintBlocks(std::mt19937_64& rng, Input& in) {
    const std::vector<double> colours = palette(rng, in.byteSamples, uniform(rng, 2, 5));
    const int64_t c = in.feature != 0 ? uniform(rng, 1, std::min<int64_t>(6, in.feature)) : uniform(rng, 2, 6);
    // Where features are small, most blocks are empty, so blocks touch less.
    const double empty = (in.feature != 0 ? 0.5 : 0.1) + 0.4 * std::uniform_real_distribution<double>(0.0, 1.0)(rng);
    const int64_t columns = (in.width + c - 1) / c;
    const int64_t rows = (in.height + c - 1) / c;
    std::vector<double> cell(static_cast<size_t>(columns * rows));
    for (double& v : cell) {
        v = chance(rng, empty) ? 0.0
                               : colours[static_cast<size_t>(uniform(rng, 0, static_cast<int64_t>(colours.size()) - 1))];
    }
    in.values.assign(static_cast<size_t>(in.width * in.height), 0.0);
    for (int64_t y = 0; y < in.height; ++y) {
        for (int64_t x = 0; x < in.width; ++x) {
            in.values[static_cast<size_t>(y * in.width + x)] = cell[static_cast<size_t>((y / c) * columns + x / c)];
        }
    }
}

// The Euclidean distance from each pixel of @p mask to the nearest pixel centre
// outside it, the outside of the level counting as background (pixels beyond
// the level are zero), clamped at @p cap when given, as pixel.distance_transform
// with maximumDistancePx would give; 0 off the mask. By brute force.
std::vector<double> distanceTransform(const std::vector<uint8_t>& mask, int64_t width, int64_t height,
                                      std::optional<double> cap) {
    std::vector<std::pair<int64_t, int64_t>> background;
    for (int64_t y = -1; y <= height; ++y) {
        for (int64_t x = -1; x <= width; ++x) {
            const bool outside = x < 0 || y < 0 || x >= width || y >= height;
            if (outside || mask[static_cast<size_t>(y * width + x)] == 0) {
                background.emplace_back(x, y);
            }
        }
    }
    std::vector<double> distance(mask.size(), 0.0);
    for (int64_t y = 0; y < height; ++y) {
        for (int64_t x = 0; x < width; ++x) {
            if (mask[static_cast<size_t>(y * width + x)] == 0) {
                continue;
            }
            int64_t best = std::numeric_limits<int64_t>::max();
            for (const auto& b : background) {
                best = std::min(best, (b.first - x) * (b.first - x) + (b.second - y) * (b.second - y));
            }
            double d = std::sqrt(static_cast<double>(best));
            if (cap) {
                d = std::min(d, *cap);
            }
            distance[static_cast<size_t>(y * width + x)] = d;
        }
    }
    return distance;
}

// Whether the centre of (x, y) lies within @p halfWidth of the segment a-b.
bool nearSegment(double x, double y, double ax, double ay, double bx, double by, double halfWidth) {
    const double dx = bx - ax;
    const double dy = by - ay;
    const double length2 = dx * dx + dy * dy;
    double t = length2 > 0.0 ? ((x - ax) * dx + (y - ay) * dy) / length2 : 0.0;
    t = std::min(1.0, std::max(0.0, t));
    const double px = ax + t * dx - x;
    const double py = ay + t * dy - y;
    return px * px + py * py <= halfWidth * halfWidth;
}

// The distance transform of random discs and strands, capped in half the inputs.
void paintDistance(std::mt19937_64& rng, Input& in) {
    std::vector<uint8_t> mask(static_cast<size_t>(in.width * in.height), 0);
    const int64_t discs = uniform(rng, 0, 6);
    const int64_t strands = discs == 0 ? uniform(rng, 1, 3) : uniform(rng, 0, 3);
    for (int64_t d = 0; d < discs; ++d) {
        const int64_t r = in.feature != 0 ? uniform(rng, 1, in.feature) : uniform(rng, 2, 14);
        const int64_t cx = uniform(rng, -r / 2, in.width - 1 + r / 2);
        const int64_t cy = uniform(rng, -r / 2, in.height - 1 + r / 2);
        for (int64_t y = 0; y < in.height; ++y) {
            for (int64_t x = 0; x < in.width; ++x) {
                if ((x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r) {
                    mask[static_cast<size_t>(y * in.width + x)] = 1;
                }
            }
        }
    }
    for (int64_t s = 0; s < strands; ++s) {
        const auto ax = static_cast<double>(uniform(rng, 0, in.width - 1));
        const auto ay = static_cast<double>(uniform(rng, 0, in.height - 1));
        // Short strands where features are small: within 2 features of the start.
        const int64_t reach = in.feature != 0 ? 2 * in.feature : std::max(in.width, in.height);
        const auto bx = ax + static_cast<double>(uniform(rng, -reach, reach));
        const auto by = ay + static_cast<double>(uniform(rng, -reach, reach));
        const double halfWidth = 0.5 + 0.5 * static_cast<double>(uniform(rng, 0, 4));
        for (int64_t y = 0; y < in.height; ++y) {
            for (int64_t x = 0; x < in.width; ++x) {
                if (nearSegment(static_cast<double>(x), static_cast<double>(y), ax, ay, bx, by, halfWidth)) {
                    mask[static_cast<size_t>(y * in.width + x)] = 1;
                }
            }
        }
    }
    std::optional<double> cap;
    if (chance(rng, 0.5)) {
        cap = static_cast<double>(uniform(rng, 1, 12));
    }
    in.values = distanceTransform(mask, in.width, in.height, cap);
}

// Anchors, each with a value of its own (some beyond 32 bits): where features
// are large, a few points or 3 x 3 squares anywhere; where they are small, about
// one foreground pixel in feature^2. Pixels on background are not markers.
std::vector<int64_t> randomAnchors(std::mt19937_64& rng, const Input& in) {
    const int64_t width = in.width;
    const int64_t height = in.height;
    std::vector<int64_t> anchors(static_cast<size_t>(width * height), -1);
    int64_t next = 0;
    // Distinct by construction: the anchor's ordinal times 1000, plus noise.
    const auto value = [&] {
        return (chance(rng, 0.3) ? int64_t{5000000000} : int64_t{0}) + 1000 * next++ + uniform(rng, 0, 999);
    };
    if (in.feature != 0) {
        const double p = 1.0 / static_cast<double>(in.feature * in.feature);
        for (size_t i = 0; i < anchors.size(); ++i) {
            if (foreground(in.values[i]) && chance(rng, p)) {
                anchors[i] = value();
            }
        }
        return anchors;
    }
    const int64_t count = uniform(rng, 0, 6);
    for (int64_t a = 0; a < count; ++a) {
        const int64_t v = value();
        const int64_t cx = uniform(rng, 0, width - 1);
        const int64_t cy = uniform(rng, 0, height - 1);
        const int64_t r = chance(rng, 0.3) ? 1 : 0;
        for (int64_t y = std::max<int64_t>(0, cy - r); y <= std::min(height - 1, cy + r); ++y) {
            for (int64_t x = std::max<int64_t>(0, cx - r); x <= std::min(width - 1, cx + r); ++x) {
                anchors[static_cast<size_t>(y * width + x)] = v;
            }
        }
    }
    return anchors;
}


Input randomInput(int32_t seed) {
    std::mt19937_64 rng(static_cast<uint64_t>(seed));
    Input in;
    in.name = "seed " + std::to_string(seed);
    in.width = uniform(rng, 16, 48);
    in.height = uniform(rng, 16, 48);
    in.byteSamples = chance(rng, 0.5);
    const int64_t spacings[] = {1, 2, 3, 4, 5, 6, 7, 8, 31};
    in.spacing = spacings[uniform(rng, 0, 8)];
    in.markerDistance = uniform(rng, 1, in.spacing);
    // Half the inputs have features about the spacing in size, which the reach
    // mostly covers (so Fail often runs and Drop is compared with it); the
    // other half up to half the level, which mostly drops.
    const bool small = chance(rng, 0.5);
    in.feature = small ? in.spacing : 0;
    if (chance(rng, 0.5)) {
        in.generator = "distance";
        paintDistance(rng, in);
    } else if (chance(rng, 0.5)) {
        in.generator = "rectangles";
        paintRectangles(rng, in);
    } else {
        in.generator = "blocks";
        paintBlocks(rng, in);
    }
    if (small) {
        in.generator += "/small";
    }
    for (double& v : in.values) {
        v = asSample(v, in.byteSamples);
    }
    // A quarter use anchors; two thirds of the rest a floor (no floor with
    // anchors, which the operation refuses), so half of all have one.
    if (chance(rng, 0.25)) {
        in.anchors = randomAnchors(rng, in);
        in.auxiliary = true;
    } else if (chance(rng, 2.0 / 3.0)) {
        std::vector<double> present;
        for (const double v : in.values) {
            if (foreground(v)) {
                present.push_back(v);
            }
        }
        // Where features are small, half the floors are the lowest value present,
        // which keeps every maximum.
        double floor = 1.0;
        if (!present.empty()) {
            floor = small && chance(rng, 0.5)
                        ? *std::min_element(present.begin(), present.end())
                        : present[static_cast<size_t>(uniform(rng, 0, static_cast<int64_t>(present.size()) - 1))];
        }
        if (chance(rng, 1.0 / 3.0)) {
            floor += in.byteSamples ? 0.5 : 0.125;
        }
        in.floor = floor;
    }
    // Every input at every size, with four workers and with one: at these sizes
    // the whole set fits the time budget, tile size 1 included, so nothing is
    // sampled.
    const std::vector<int32_t> roles = roleTiles(in.spacing);
    for (const int32_t t : std::set<int32_t>(roles.begin(), roles.end())) {
        in.runs.push_back({t, 4});
        in.runs.push_back({t, 1});
    }
    return in;
}

// ---------------------------------------------------------------------------
// Comparison

enum class Kind {
    TileDependence,   // (a) tiled Drop differs from single-tile Drop
    OperationVsModel, // (b) single-tile operation differs from the model
    DropVsFail,       // (c) Drop differs from Fail where Fail runs at every size
    Harness           // the harness itself failed (an unexpected exception)
};
const char* const kKindNames[] = {"(a) tile-dependence", "(b) operation vs model", "(c) Drop vs robust Fail",
                                  "harness error"};

struct Finding {
    Kind kind;
    std::string detail;
};

struct Outcome {
    std::vector<Finding> findings;
    std::vector<int32_t> failPass;  // tile sizes at which Fail ran (kSingleTile: one tile)
    std::vector<int32_t> failThrow; // tile sizes at which Fail refused
    int64_t dropped = 0;            // single-tile Drop's dropped pixels
    bool modelRefused = false;
    std::vector<TiledRun> ran;
    std::vector<TiledRun> tileDependent; // the tiled Drop runs that differ from one tile
    // Which of each class's two checks failed, on any run of the input:
    // (a) objects / dropped set; (b) partition / dropped set; (c) dropped
    // anything / objects.
    bool firstCheck[3] = {false, false, false};
    bool secondCheck[3] = {false, false, false};
};

std::string messageOf(const std::exception& e) {
    if (const auto* p = dynamic_cast<const ProcessorError*>(&e)) {
        if (p->cause() != nullptr && p->cause()[0] != '\0') {
            return p->cause();
        }
    }
    return e.what();
}

bool isReachError(const std::string& message) {
    return message.find("maximumMarkerSpacingPx") != std::string::npos &&
           message.rfind("segment.watershed: ", 0) == 0;
}

PipelineSpec specOf(const Input& in, WatershedUnreached unreached) {
    WatershedParameters p;
    p.minimumMarkerDistancePx(static_cast<int32_t>(in.markerDistance))
        .maximumMarkerSpacingPx(static_cast<int32_t>(in.spacing))
        .unreachedForeground(unreached);
    if (in.floor) {
        p.minimumMarkerValue(*in.floor);
    }
    if (!in.anchors.empty()) {
        p.markers(WatershedMarkers::AuxiliaryRaster).auxiliaryName("anchors");
    }
    PipelineSpec spec;
    spec.then(makeWatershed(p));
    return spec;
}

// The pixels a run dropped: label 0 on foreground.
std::vector<uint8_t> droppedMask(const ts::Image& labels, const std::vector<double>& values) {
    std::vector<uint8_t> mask(values.size(), 0);
    for (int64_t y = 0; y < labels.height; ++y) {
        for (int64_t x = 0; x < labels.width; ++x) {
            const size_t i = static_cast<size_t>(y * labels.width + x);
            mask[i] = foreground(values[i]) && labels.at(0, x, y) == 0.0 ? 1 : 0;
        }
    }
    return mask;
}

// Empty when the masks agree; else how many pixels differ and the first.
std::string maskDifference(const std::vector<uint8_t>& want, const std::vector<uint8_t>& got, int64_t width) {
    int64_t count = 0;
    int64_t first = -1;
    for (size_t i = 0; i < want.size() && i < got.size(); ++i) {
        if (want[i] != got[i]) {
            ++count;
            if (first < 0) {
                first = static_cast<int64_t>(i);
            }
        }
    }
    if (want.size() != got.size()) {
        return "the masks differ in size";
    }
    if (count == 0) {
        return std::string();
    }
    std::ostringstream out;
    out << count << " pixels differ, first (" << first % width << ", " << first / width << ") "
        << (got[static_cast<size_t>(first)] != 0 ? "dropped" : "kept") << " here, "
        << (want[static_cast<size_t>(first)] != 0 ? "dropped" : "kept") << " in the reference";
    return out.str();
}

std::string runName(int32_t tile, int32_t workers) {
    return std::to_string(tile) + "-px tiles, " + std::to_string(workers) + (workers == 1 ? " worker" : " workers");
}

// Each object as one line: bounding box, rings, area.
std::string listing(const std::vector<ObjectRecord>& objects) {
    std::ostringstream out;
    for (const ObjectRecord& r : objects) {
        const Rect64 b = r.boundingBox();
        Measurement area;
        r.measurements().find("area", area);
        out << "      box (" << b.x << ", " << b.y << ") " << b.width << "x" << b.height << ", rings "
            << r.geometry().ringCount() << ", area " << area.value() << '\n';
    }
    return out.str();
}

// A raster of single characters: '.' background, '#' kept, 'x' dropped.
std::string maskPicture(const std::vector<uint8_t>& dropped, const std::vector<double>& values, int64_t width) {
    std::ostringstream out;
    for (size_t i = 0; i < values.size(); ++i) {
        out << (!foreground(values[i]) ? '.' : dropped[i] != 0 ? 'x' : '#');
        if (static_cast<int64_t>(i % static_cast<size_t>(width)) == width - 1) {
            out << '\n';
        }
    }
    return out.str();
}

// With @p verbose, every mismatching run's objects (and both dropped masks) are
// printed to stdout as well: the diagnosis of one input.
Outcome checkCase(const Input& in, bool verbose = false) {
    Outcome outcome;
    const auto add = [&](Kind kind, const std::string& detail) { outcome.findings.push_back({kind, detail}); };
    const int64_t width = in.width;
    const int64_t height = in.height;
    auto values = std::make_shared<const std::vector<double>>(in.values);
    const auto scene = ts::makeSyntheticScene(
        {{width, height, 0, 0}}, {in.byteSamples ? slideio::DataType::DT_Byte : slideio::DataType::DT_Float32},
        [values, width, height](int, int64_t x, int64_t y) {
            return x >= 0 && y >= 0 && x < width && y < height ? (*values)[static_cast<size_t>(y * width + x)] : 0.0;
        });
    std::unique_ptr<ts::AnchorSource> anchors;
    if (!in.anchors.empty()) {
        auto table = std::make_shared<const std::vector<int64_t>>(in.anchors);
        anchors = std::make_unique<ts::AnchorSource>([table, width, height](int64_t x, int64_t y) -> int64_t {
            return x >= 0 && y >= 0 && x < width && y < height ? (*table)[static_cast<size_t>(y * width + x)] : -1;
        });
    }
    const PipelineSpec drop = specOf(in, WatershedUnreached::Drop);
    const PipelineSpec fail = specOf(in, WatershedUnreached::Fail);

    ts::ModelParameters mp;
    mp.markerDistance = in.markerDistance;
    mp.spacing = in.spacing;
    mp.floor = in.floor;
    mp.anchors = in.anchors;
    mp.rule = kDropRule;
    const ts::ModelResult model = ts::modelWatershed(in.values, width, height, mp);
    mp.rule = ts::ModelRule::Fail;
    outcome.modelRefused = ts::modelWatershed(in.values, width, height, mp).refused;
    std::vector<uint8_t> modelDropped(in.values.size(), 0);
    for (size_t i = 0; i < model.key.size(); ++i) {
        modelDropped[i] = model.key[i] == ts::kModelDropped ? 1 : 0;
    }

    // 1. Single-tile Drop against the model.
    ts::EagerRun single;
    try {
        single = ts::runEagerAll(scene, drop, kSingleTile, 4, anchors.get());
    } catch (const std::exception& e) {
        add(Kind::OperationVsModel, "single-tile Drop threw: " + messageOf(e));
        return outcome;
    }
    const std::vector<uint8_t> singleDropped = droppedMask(single.raster, in.values);
    outcome.dropped = std::count(singleDropped.begin(), singleDropped.end(), uint8_t{1});
    const std::string partition = ts::partitionDifference(model.key, single.raster);
    if (!partition.empty()) {
        add(Kind::OperationVsModel, std::string("single-tile Drop vs model ") + kDropRuleName + " partition: " + partition);
        outcome.firstCheck[1] = true;
    }
    const std::string modelMask = maskDifference(modelDropped, singleDropped, width);
    if (!modelMask.empty()) {
        add(Kind::OperationVsModel, std::string("single-tile Drop vs model ") + kDropRuleName + " dropped set: " + modelMask);
        outcome.secondCheck[1] = true;
    }

    // 2. Tiled Drop against single-tile Drop.
    for (const TiledRun& run : in.runs) {
        outcome.ran.push_back(run);
        bool differs = false;
        try {
            const ts::EagerRun tiled = ts::runEagerAll(scene, drop, run.tile, run.workers, anchors.get());
            const std::string objects = ts::objectsDifference(single.objects, tiled.objects);
            const std::vector<uint8_t> tiledDropped = droppedMask(tiled.raster, in.values);
            const std::string mask = maskDifference(singleDropped, tiledDropped, width);
            differs = !objects.empty() || !mask.empty();
            if (!objects.empty()) {
                add(Kind::TileDependence, runName(run.tile, run.workers) + ": objects: " + objects);
                outcome.firstCheck[0] = true;
                if (verbose) {
                    std::cout << "    " << runName(run.tile, run.workers) << ": " << objects << '\n'
                              << "    single tile:\n"
                              << listing(single.objects) << "    tiled:\n"
                              << listing(tiled.objects);
                }
            }
            if (!mask.empty()) {
                add(Kind::TileDependence, runName(run.tile, run.workers) + ": dropped set: " + mask);
                outcome.secondCheck[0] = true;
                if (verbose) {
                    std::cout << "    " << runName(run.tile, run.workers) << ": " << mask << '\n'
                              << "    single tile:\n"
                              << maskPicture(singleDropped, in.values, width) << "    tiled:\n"
                              << maskPicture(tiledDropped, in.values, width);
                }
            }
        } catch (const std::exception& e) {
            // Drop never refuses for reach, so a throw is a harness or pipeline
            // error, not tile-dependence.
            add(Kind::Harness, runName(run.tile, run.workers) + ": tiled Drop threw: " + messageOf(e));
        }
        if (differs) {
            outcome.tileDependent.push_back(run);
        }
    }

    // 3. Fail at one tile and at every tiled size, four workers.
    std::set<int32_t> failTiles{kSingleTile};
    for (const TiledRun& run : in.runs) {
        failTiles.insert(run.tile);
    }
    std::vector<std::pair<int32_t, std::vector<ObjectRecord>>> failObjects;
    for (const int32_t tile : failTiles) {
        try {
            std::vector<ObjectRecord> objects = anchors ? ts::runWithAnchors(scene, fail, tile, *anchors, 4)
                                                        : ts::runEagerObjects(scene, fail, tile, 4);
            outcome.failPass.push_back(tile);
            failObjects.emplace_back(tile, std::move(objects));
        } catch (const std::exception& e) {
            const std::string message = messageOf(e);
            if (!isReachError(message)) {
                add(Kind::Harness, "Fail at " + std::to_string(tile) + "-px tiles threw a non-reach error: " + message);
            }
            outcome.failThrow.push_back(tile);
        }
    }
    const bool singleFailed =
        std::find(outcome.failThrow.begin(), outcome.failThrow.end(), kSingleTile) != outcome.failThrow.end();
    if (singleFailed != outcome.modelRefused) {
        add(Kind::OperationVsModel, std::string("single-tile Fail ") + (singleFailed ? "refused" : "ran") +
                                        ", the model Fail " + (outcome.modelRefused ? "refuses" : "runs"));
    }
    if (outcome.failThrow.empty()) {
        if (outcome.dropped != 0) {
            outcome.firstCheck[2] = true;
            add(Kind::DropVsFail, "Fail runs at every size, Drop dropped " + std::to_string(outcome.dropped) +
                                      " pixels over one tile");
        }
        for (const auto& [tile, objects] : failObjects) {
            const std::string difference = ts::objectsDifference(objects, single.objects);
            if (!difference.empty()) {
                outcome.secondCheck[2] = true;
                add(Kind::DropVsFail, "single-tile Drop vs Fail at " + std::to_string(tile) + "-px tiles: " + difference);
            }
        }
    }
    return outcome;
}

Outcome checkCaseGuarded(const Input& in) {
    try {
        return checkCase(in);
    } catch (const std::exception& e) {
        Outcome outcome;
        outcome.findings.push_back({Kind::Harness, std::string("exception: ") + e.what()});
        return outcome;
    }
}

// Runs make(i) and checkCase on it for i in [0, count), one input per thread.
template <typename Make>
std::vector<std::pair<Input, Outcome>> runAll(int32_t count, Make make) {
    std::vector<std::pair<Input, Outcome>> results(static_cast<size_t>(count));
    std::atomic<int32_t> next{0};
    const unsigned hardware = std::max(1u, std::thread::hardware_concurrency());
    const unsigned threads = std::max(1u, std::min<unsigned>(hardware / 2, static_cast<unsigned>(count)));
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < threads; ++t) {
        pool.emplace_back([&] {
            for (int32_t i = next++; i < count; i = next++) {
                Input in = make(i);
                Outcome outcome = checkCaseGuarded(in);
                in.values.clear();
                in.values.shrink_to_fit();
                in.anchors.clear();
                in.anchors.shrink_to_fit();
                results[static_cast<size_t>(i)] = {std::move(in), std::move(outcome)};
            }
        });
    }
    for (std::thread& t : pool) {
        t.join();
    }
    return results;
}

std::string joinTiles(const std::vector<int32_t>& tiles) {
    std::ostringstream out;
    for (size_t i = 0; i < tiles.size(); ++i) {
        out << (i ? "," : "") << (tiles[i] == kSingleTile ? std::string("single") : std::to_string(tiles[i]));
    }
    return out.str();
}

std::string describe(const Input& in) {
    std::ostringstream out;
    out << in.name << " [" << in.generator << ", " << in.width << "x" << in.height << ", "
        << (in.byteSamples ? "Byte" : "Float32") << ", spacing " << in.spacing << ", markerDistance "
        << in.markerDistance << ", " << (in.auxiliary ? "AuxiliaryRaster" : "LocalMaxima");
    if (in.floor) {
        out << ", floor " << *in.floor;
    }
    out << "]";
    return out.str();
}

struct Summary {
    std::string text;
    int64_t inputsWithKind[4] = {0, 0, 0, 0};
    int64_t failModeFindings = 0;
};

template <typename Key>
void countLine(std::ostringstream& out, const char* title, const std::map<Key, int64_t>& counts) {
    out << "  " << title << ":";
    for (const auto& [key, count] : counts) {
        out << " " << key << "=" << count;
    }
    out << "\n";
}

// The summary the brief asks for: coverage, mismatches by kind with the inputs
// that show them, and Fail-mode findings.
Summary summarise(const std::string& title, const std::vector<std::pair<Input, Outcome>>& results, double seconds,
                  bool byRole) {
    Summary summary;
    std::ostringstream out;
    out << "==== " << title << ": " << results.size() << " inputs, Drop against model " << kDropRuleName << ", " << seconds << " s wall ====\n";
    out << "coverage (inputs):\n";
    if (byRole) {
        // Per tile-size role and worker count: inputs run with that role's size.
        for (size_t r = 0; r < 6; ++r) {
            int64_t four = 0;
            int64_t one = 0;
            for (const auto& [in, outcome] : results) {
                const int32_t tile = roleTiles(in.spacing)[r];
                const auto ran = [&](int32_t workers) {
                    return std::any_of(outcome.ran.begin(), outcome.ran.end(),
                                       [&](const TiledRun& t) { return t.tile == tile && t.workers == workers; });
                };
                four += ran(4) ? 1 : 0;
                one += ran(1) ? 1 : 0;
            }
            int64_t dependent = 0;
            for (const auto& [in, outcome] : results) {
                const int32_t tile = roleTiles(in.spacing)[r];
                dependent += std::any_of(outcome.tileDependent.begin(), outcome.tileDependent.end(),
                                         [&](const TiledRun& t) { return t.tile == tile; })
                                 ? 1
                                 : 0;
            }
            // Tiled Drop runs at this role's size, both worker counts, and those
            // in which some tile's halo'd context is smaller than the level.
            int64_t roleRuns = 0;
            int64_t rolePartial = 0;
            for (const auto& [in, outcome] : results) {
                const int32_t tile = roleTiles(in.spacing)[r];
                for (const TiledRun& t : outcome.ran) {
                    if (t.tile == tile) {
                        ++roleRuns;
                        rolePartial +=
                            someContextSmallerThanLevel(in.width, in.height, t.tile, dropHalo(in.spacing)) ? 1 : 0;
                    }
                }
            }
            out << "  tile " << kRoleNames[r] << ": 4 workers " << four << ", 1 worker " << one
                << "; inputs tile-dependent at this size: " << dependent << "; tiled Drop runs " << roleRuns
                << ", with a context smaller than the level " << rolePartial << "\n";
        }
    }
    std::map<int32_t, int64_t> tilesFour;
    std::map<int32_t, int64_t> tilesOne;
    std::map<int64_t, int64_t> spacing;
    std::map<std::string, int64_t> markers;
    std::map<std::string, int64_t> floor;
    std::map<std::string, int64_t> type;
    std::map<std::string, int64_t> generator;
    std::map<std::string, int64_t> robustByGenerator;
    std::map<int64_t, int64_t> robustBySpacing;
    std::map<int64_t, int64_t> dependentBySpacing;
    std::map<std::string, int64_t> dependentByMarkers;
    std::map<std::string, int64_t> dependentByFloor;
    std::map<std::string, int64_t> dependentByGenerator;
    int64_t workersDisagree = 0;
    // Per class (a), (b), (c): inputs where only the first check failed, only
    // the second, or both.
    int64_t firstOnly[3] = {0, 0, 0};
    int64_t secondOnly[3] = {0, 0, 0};
    int64_t bothChecks[3] = {0, 0, 0};
    int64_t withDrops = 0;
    int64_t droppedPixels = 0;
    int64_t modelRefused = 0;
    int64_t failRobust = 0;
    int64_t failNowhere = 0;
    int64_t runs = 0;
    // Tiled Drop runs per spacing (both worker counts), and those in which some
    // tile's halo'd context is smaller than the level.
    std::map<int64_t, std::pair<int64_t, int64_t>> contextBySpacing;
    std::vector<std::string> failMode;
    std::vector<std::string> byKind[4];
    std::vector<std::string> namesByKind[4];
    for (const auto& [in, outcome] : results) {
        std::set<int32_t> four;
        std::set<int32_t> one;
        for (const TiledRun& t : outcome.ran) {
            (t.workers == 1 ? one : four).insert(t.tile);
        }
        runs += static_cast<int64_t>(outcome.ran.size() + outcome.failPass.size() + outcome.failThrow.size()) + 1;
        for (const int32_t t : four) {
            ++tilesFour[t];
        }
        for (const int32_t t : one) {
            ++tilesOne[t];
        }
        ++spacing[in.spacing];
        for (const TiledRun& t : outcome.ran) {
            auto& counts = contextBySpacing[in.spacing];
            ++counts.first;
            counts.second += someContextSmallerThanLevel(in.width, in.height, t.tile, dropHalo(in.spacing)) ? 1 : 0;
        }
        ++markers[in.auxiliary ? "AuxiliaryRaster" : "LocalMaxima"];
        ++floor[in.floor ? "on" : "off"];
        ++type[in.byteSamples ? "Byte" : "Float32"];
        ++generator[in.generator];
        robustByGenerator[in.generator] += outcome.failThrow.empty() ? 1 : 0;
        robustBySpacing[in.spacing] += outcome.failThrow.empty() ? 1 : 0;
        if (!outcome.tileDependent.empty()) {
            ++dependentBySpacing[in.spacing];
            ++dependentByMarkers[in.auxiliary ? "AuxiliaryRaster" : "LocalMaxima"];
            ++dependentByFloor[in.floor ? "on" : "off"];
            ++dependentByGenerator[in.generator];
            // The same tile sizes differ with one worker as with four, unless
            // the run is not even deterministic.
            std::set<int32_t> dependentFour;
            std::set<int32_t> dependentOne;
            for (const TiledRun& t : outcome.tileDependent) {
                (t.workers == 1 ? dependentOne : dependentFour).insert(t.tile);
            }
            workersDisagree += dependentFour != dependentOne ? 1 : 0;
        }
        for (size_t k = 0; k < 3; ++k) {
            firstOnly[k] += outcome.firstCheck[k] && !outcome.secondCheck[k] ? 1 : 0;
            secondOnly[k] += !outcome.firstCheck[k] && outcome.secondCheck[k] ? 1 : 0;
            bothChecks[k] += outcome.firstCheck[k] && outcome.secondCheck[k] ? 1 : 0;
        }
        withDrops += outcome.dropped > 0 ? 1 : 0;
        droppedPixels += outcome.dropped;
        modelRefused += outcome.modelRefused ? 1 : 0;
        failRobust += outcome.failThrow.empty() ? 1 : 0;
        failNowhere += outcome.failPass.empty() ? 1 : 0;
        if (!outcome.failPass.empty() && !outcome.failThrow.empty()) {
            failMode.push_back(describe(in) + ": Fail runs at " + joinTiles(outcome.failPass) + ", refuses at " +
                               joinTiles(outcome.failThrow));
        }
        bool seen[4] = {false, false, false, false};
        for (const Finding& f : outcome.findings) {
            const auto k = static_cast<size_t>(f.kind);
            if (!seen[k]) {
                seen[k] = true;
                byKind[k].push_back(describe(in) + ": " + f.detail);
                namesByKind[k].push_back(in.name);
            }
        }
    }
    countLine(out, "tile sizes, 4 workers", tilesFour);
    out << "  tiled Drop runs by spacing (runs / with a halo'd context smaller than the level):";
    for (const auto& [s, counts] : contextBySpacing) {
        out << " " << s << "=" << counts.first << "/" << counts.second;
    }
    out << "\n";
    countLine(out, "tile sizes, 1 worker", tilesOne);
    countLine(out, "spacing", spacing);
    countLine(out, "markers", markers);
    countLine(out, "floor", floor);
    countLine(out, "type", type);
    countLine(out, "generator", generator);
    countLine(out, "Fail runs at every size, by generator", robustByGenerator);
    countLine(out, "Fail runs at every size, by spacing", robustBySpacing);
    out << "  pipeline runs: " << runs << "\n";
    out << "  inputs where single-tile Drop drops: " << withDrops << " (" << droppedPixels << " pixels)\n";
    out << "  inputs the model's Fail refuses: " << modelRefused << "\n";
    out << "  inputs Fail runs at every size: " << failRobust << ", at no size: " << failNowhere << "\n";
    out << "mismatches (inputs):\n";
    countLine(out, "(a) by spacing", dependentBySpacing);
    countLine(out, "(a) by markers", dependentByMarkers);
    countLine(out, "(a) by floor", dependentByFloor);
    countLine(out, "(a) by generator", dependentByGenerator);
    out << "  (a) inputs whose tile-dependent sizes differ between 1 and 4 workers: " << workersDisagree << "\n";
    const char* const checkNames[3][3] = {{"(a)", "objects", "dropped set"},
                                          {"(b)", "partition", "dropped set"},
                                          {"(c)", "Drop dropped pixels", "objects"}};
    for (size_t k = 0; k < 3; ++k) {
        out << "  " << checkNames[k][0] << " inputs: " << checkNames[k][1] << " only " << firstOnly[k] << ", "
            << checkNames[k][2] << " only " << secondOnly[k] << ", both " << bothChecks[k] << "\n";
    }
    for (size_t k = 0; k < 4; ++k) {
        summary.inputsWithKind[k] = static_cast<int64_t>(byKind[k].size());
        out << "  " << kKindNames[k] << ": " << byKind[k].size() << "\n";
        if (!byKind[k].empty()) {
            out << "    inputs:";
            for (size_t i = 0; i < namesByKind[k].size() && i < 200; ++i) {
                out << (i ? ", " : " ") << namesByKind[k][i];
            }
            out << (namesByKind[k].size() > 200 ? ", ..." : "") << "\n";
            for (size_t i = 0; i < byKind[k].size() && i < 40; ++i) {
                out << "    " << byKind[k][i] << "\n";
            }
        }
    }
    summary.failModeFindings = static_cast<int64_t>(failMode.size());
    out << "Fail-mode findings (Fail runs at some sizes, refuses at others; not a failure): " << failMode.size()
        << "\n";
    for (size_t i = 0; i < failMode.size() && i < 40; ++i) {
        out << "  " << failMode[i] << "\n";
    }
    if (failMode.size() > 40) {
        out << "  ...\n";
    }
    summary.text = out.str();
    return summary;
}

void report(const std::string& title, const std::vector<std::pair<Input, Outcome>>& results, double seconds,
            bool byRole) {
    const Summary summary = summarise(title, results, seconds, byRole);
    std::cout << summary.text << std::flush;
    ::testing::Test::RecordProperty("inputs", static_cast<int>(results.size()));
    ::testing::Test::RecordProperty("mismatches_a_tile_dependence", static_cast<int>(summary.inputsWithKind[0]));
    ::testing::Test::RecordProperty("mismatches_b_operation_vs_model", static_cast<int>(summary.inputsWithKind[1]));
    ::testing::Test::RecordProperty("mismatches_c_drop_vs_fail", static_cast<int>(summary.inputsWithKind[2]));
    ::testing::Test::RecordProperty("harness_errors", static_cast<int>(summary.inputsWithKind[3]));
    ::testing::Test::RecordProperty("fail_mode_findings", static_cast<int>(summary.failModeFindings));
    ::testing::Test::RecordProperty("wall_seconds", static_cast<int>(std::lround(seconds)));
    ::testing::Test::RecordProperty("summary", summary.text);
    for (size_t k = 0; k < 4; ++k) {
        EXPECT_EQ(summary.inputsWithKind[k], 0) << kKindNames[k] << " (see the summary on stdout)";
    }
}

// ---------------------------------------------------------------------------
// Generated shapes

Input shapeInput(const std::string& name, int64_t width, int64_t height, bool byteSamples,
                 const std::vector<double>& raw, int64_t spacing, int64_t markerDistance,
                 const std::vector<int32_t>& extraTiles) {
    Input in;
    in.name = name;
    in.generator = "shape";
    in.width = width;
    in.height = height;
    in.byteSamples = byteSamples;
    in.values = raw;
    for (double& v : in.values) {
        v = asSample(v, byteSamples);
    }
    in.spacing = spacing;
    in.markerDistance = markerDistance;
    std::set<int32_t> tiles(extraTiles.begin(), extraTiles.end());
    for (const int32_t t : roleTiles(spacing)) {
        tiles.insert(t);
    }
    for (const int32_t t : tiles) {
        in.runs.push_back({t, 4});
        in.runs.push_back({t, 1});
    }
    return in;
}

std::vector<uint8_t> discMask(int64_t width, int64_t height, const std::vector<std::array<int64_t, 3>>& discs) {
    std::vector<uint8_t> mask(static_cast<size_t>(width * height), 0);
    for (int64_t y = 0; y < height; ++y) {
        for (int64_t x = 0; x < width; ++x) {
            for (const auto& d : discs) {
                if ((x - d[0]) * (x - d[0]) + (y - d[1]) * (y - d[1]) <= d[2] * d[2]) {
                    mask[static_cast<size_t>(y * width + x)] = 1;
                }
            }
        }
    }
    return mask;
}

std::vector<Input> generatedShapes() {
    std::vector<Input> shapes;

    // A strand longer than the spacing, at 0, 45 and 90 degrees, shifted so the
    // seams of every tile size cross it in different places: as the distance
    // transform of a thick line (a plateau ridge, one marker at its first
    // pixel), floored to Byte too, and as a ramp falling from one end.
    {
        constexpr int64_t kSide = 64;
        for (const int angle : {0, 45, 90}) {
            for (const int64_t offset : {0, 5, 13}) {
                double ax = 6;
                double ay = 6;
                double bx = 6;
                double by = 6;
                if (angle == 0) {
                    ay = by = static_cast<double>(20 + offset);
                    bx = 56;
                } else if (angle == 45) {
                    ax = static_cast<double>(6 + offset);
                    bx = ax + 40;
                    by = 46;
                } else {
                    ax = bx = static_cast<double>(20 + offset);
                    by = 56;
                }
                std::vector<uint8_t> mask(static_cast<size_t>(kSide * kSide), 0);
                std::vector<double> ramp(static_cast<size_t>(kSide * kSide), 0.0);
                for (int64_t y = 0; y < kSide; ++y) {
                    for (int64_t x = 0; x < kSide; ++x) {
                        const auto fx = static_cast<double>(x);
                        const auto fy = static_cast<double>(y);
                        if (nearSegment(fx, fy, ax, ay, bx, by, 1.5)) {
                            mask[static_cast<size_t>(y * kSide + x)] = 1;
                        }
                        if (nearSegment(fx, fy, ax, ay, bx, by, 1.0)) {
                            const double along = std::hypot(fx - ax, fy - ay);
                            ramp[static_cast<size_t>(y * kSide + x)] = 200.0 - std::floor(along);
                        }
                    }
                }
                const std::vector<double> ridge = distanceTransform(mask, kSide, kSide, std::nullopt);
                const std::string where = "strand " + std::to_string(angle) + " deg, offset " + std::to_string(offset);
                const std::vector<int32_t> extra = {16, 32, 64};
                shapes.push_back(shapeInput(where + ", ridge Float32", kSide, kSide, false, ridge, 8, 2, extra));
                shapes.push_back(shapeInput(where + ", ridge Byte", kSide, kSide, true, ridge, 8, 2, extra));
                shapes.push_back(shapeInput(where + ", ramp Float32", kSide, kSide, false, ramp, 8, 2, extra));
            }
        }
    }

    // An unmarked blob: a floor above its peak, or anchors that mark nothing.
    {
        constexpr int64_t kSide = 48;
        const std::vector<double> blob = distanceTransform(discMask(kSide, kSide, {{{24, 24, 12}}}), kSide, kSide,
                                                           std::nullopt);
        Input floored = shapeInput("unmarked blob, floor above its peak", kSide, kSide, false, blob, 4, 2, {16});
        floored.floor = 1000.0;
        shapes.push_back(floored);
        Input unanchored = shapeInput("unmarked blob, no anchor", kSide, kSide, false, blob, 4, 2, {16});
        unanchored.anchors.assign(blob.size(), -1);
        unanchored.auxiliary = true;
        shapes.push_back(unanchored);
    }

    // The blob touching a nucleus (the design's suspected counter-example): a
    // disc of radius 18 whose peak F is its centre, more than 2 s + 1 from a
    // nucleus of radius 4 overlapping its rim, whose own peak N gives it a
    // marker. Every tile size from 1 to 16 and some larger ones, so that some
    // contexts holding a pixel near the nucleus hold F and some do not.
    {
        constexpr int64_t kSide = 72;
        constexpr int64_t kCentre = 30;
        constexpr int64_t kBlob = 18;
        for (const int angle : {0, 45, 90}) {
            const double a = static_cast<double>(angle) * 3.14159265358979323846 / 180.0;
            const int64_t nx = kCentre + std::llround(static_cast<double>(kBlob + 3) * std::cos(a));
            const int64_t ny = kCentre + std::llround(static_cast<double>(kBlob + 3) * std::sin(a));
            const std::vector<uint8_t> mask = discMask(kSide, kSide, {{{kCentre, kCentre, kBlob}}, {{nx, ny, 4}}});
            for (const std::optional<double> cap : {std::optional<double>(), std::optional<double>(6.0)}) {
                const std::vector<double> dt = distanceTransform(mask, kSide, kSide, cap);
                for (const int64_t s : {3, 4, 5, 6}) {
                    std::vector<int32_t> extra;
                    for (int32_t t = 1; t <= 16; ++t) {
                        extra.push_back(t);
                    }
                    for (const int32_t t : {20, 24, 32}) {
                        extra.push_back(t);
                    }
                    for (const bool byteSamples : {false, true}) {
                        const std::string name = "blob touching nucleus, " + std::to_string(angle) + " deg, " +
                                                 (cap ? "capped at 6, " : "") + "spacing " + std::to_string(s) +
                                                 (byteSamples ? ", Byte" : ", Float32");
                        shapes.push_back(shapeInput(name, kSide, kSide, byteSamples, dt, s, 2, extra));
                    }
                }
            }
        }
    }

    // A 64-px disc (radius 32) at spacing 31: its rim lies beyond the spacing of
    // its one marker.
    {
        constexpr int64_t kSide = 80;
        const std::vector<double> disc = distanceTransform(discMask(kSide, kSide, {{{40, 40, 32}}}), kSide, kSide,
                                                           std::nullopt);
        for (const int64_t markerDistance : {5, 31}) {
            for (const bool byteSamples : {false, true}) {
                shapes.push_back(shapeInput("64-px disc, spacing 31, markerDistance " + std::to_string(markerDistance) +
                                                (byteSamples ? ", Byte" : ", Float32"),
                                            kSide, kSide, byteSamples, disc, 31, markerDistance, {}));
            }
        }
    }
    return shapes;
}

// The value of environment variable @p name, empty when unset.
std::string environment(const char* name) {
#ifdef _MSC_VER
    char* value = nullptr;
    size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) {
        return std::string();
    }
    std::string text(value);
    std::free(value);
    return text;
#else
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string();
#endif
}

double secondsSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

// The generated shape called @p name.
Input shapeNamed(const std::string& name) {
    for (Input& in : generatedShapes()) {
        if (in.name == name) {
            return in;
        }
    }
    throw std::invalid_argument("no generated shape is called '" + name + "'");
}

// One regression input: every comparison of checkCase must hold.
void expectNoFindings(const Input& in) {
    const Outcome outcome = checkCase(in);
    for (const Finding& f : outcome.findings) {
        ADD_FAILURE() << describe(in) << ": " << kKindNames[static_cast<size_t>(f.kind)] << ": " << f.detail;
    }
}

} // namespace

TEST(WatershedModel, RandomInputs) {
    const auto start = std::chrono::steady_clock::now();
    const auto results = runAll(kRandomInputs, [](int32_t i) { return randomInput(i); });
    report("WatershedModel.RandomInputs (seeds 0.." + std::to_string(kRandomInputs - 1) + ")", results,
           secondsSince(start), true);
}

TEST(WatershedModel, GeneratedShapes) {
    const auto start = std::chrono::steady_clock::now();
    const std::vector<Input> shapes = generatedShapes();
    const auto results = runAll(static_cast<int32_t>(shapes.size()),
                                [&shapes](int32_t i) { return shapes[static_cast<size_t>(i)]; });
    report("WatershedModel.GeneratedShapes", results, secondsSince(start), false);
}

// Regression inputs: ones the plain Drop rule (Task 4) found tile-dependent,
// class (a), each named after its seed or shape and its generator.
TEST(WatershedModel, RegressionSeed1DistanceFloor) { expectNoFindings(randomInput(1)); }
TEST(WatershedModel, RegressionSeed8DistanceAnchors) { expectNoFindings(randomInput(8)); }
TEST(WatershedModel, RegressionSeed13Rectangles) { expectNoFindings(randomInput(13)); }
TEST(WatershedModel, RegressionSeed14RectanglesFloor) { expectNoFindings(randomInput(14)); }
TEST(WatershedModel, RegressionSeed22RectanglesAnchors) { expectNoFindings(randomInput(22)); }
TEST(WatershedModel, RegressionSeed26Blocks) { expectNoFindings(randomInput(26)); }
TEST(WatershedModel, RegressionSeed188DistanceSmall) { expectNoFindings(randomInput(188)); }
// Inputs bounded claims alone (design step 4a) left tile-dependent, at
// Drop's old halo of 2 * spacing + 1.
TEST(WatershedModel, RegressionSeed176Blocks) { expectNoFindings(randomInput(176)); }
TEST(WatershedModel, RegressionSeed433RectanglesFloor) { expectNoFindings(randomInput(433)); }
TEST(WatershedModel, RegressionSeed695Distance) { expectNoFindings(randomInput(695)); }
TEST(WatershedModel, RegressionSeed1496DistanceSmallAnchors) { expectNoFindings(randomInput(1496)); }
TEST(WatershedModel, RegressionSeed3235RectanglesAnchors) { expectNoFindings(randomInput(3235)); }
// shape:30 in Task 4's numbering, the design's counter-example reproduced.
TEST(WatershedModel, RegressionShapeBlobNucleus0DegSpacing3Byte) {
    expectNoFindings(shapeNamed("blob touching nucleus, 0 deg, spacing 3, Byte"));
}
TEST(WatershedModel, RegressionShapeBlobNucleus45DegCappedSpacing3Float32) {
    expectNoFindings(shapeNamed("blob touching nucleus, 45 deg, capped at 6, spacing 3, Float32"));
}
TEST(WatershedModel, RegressionShapeBlobNucleus90DegSpacing6Byte) {
    expectNoFindings(shapeNamed("blob touching nucleus, 90 deg, spacing 6, Byte"));
}

// Not part of the gate: the diagnosis of one input, every finding and every
// mismatching run's objects and masks, when SLIDEIO_WATERSHED_MODEL_SEED names a
// random input's seed, or "shape:<i>" the i-th generated shape (from 0, in the
// order GeneratedShapes lists them). Skipped otherwise.
TEST(WatershedModel, DiagnoseSeed) {
    const std::string seed = environment("SLIDEIO_WATERSHED_MODEL_SEED");
    if (seed.empty()) {
        GTEST_SKIP() << "set SLIDEIO_WATERSHED_MODEL_SEED to diagnose one input";
    }
    const std::string shapePrefix = "shape:";
    Input in = seed.rfind(shapePrefix, 0) == 0
                   ? generatedShapes().at(static_cast<size_t>(std::stoi(seed.substr(shapePrefix.size()))))
                   : randomInput(std::stoi(seed));
    std::cout << describe(in) << "\n";
    for (int64_t y = 0; y < in.height; ++y) {
        for (int64_t x = 0; x < in.width; ++x) {
            std::cout << (x ? " " : "  ") << in.values[static_cast<size_t>(y * in.width + x)];
        }
        std::cout << "\n";
    }
    const Outcome outcome = checkCase(in, true);
    std::cout << "single-tile dropped " << outcome.dropped << ", Fail runs at " << joinTiles(outcome.failPass)
              << ", refuses at " << joinTiles(outcome.failThrow) << "\n";
    for (const Finding& f : outcome.findings) {
        std::cout << "  " << kKindNames[static_cast<size_t>(f.kind)] << ": " << f.detail << "\n";
    }
}
