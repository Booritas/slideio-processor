// Requirements: UR-SEG-3, UR-EXT-4, UR-QA-3
#include <algorithm/support/disks_operation.hpp>

#include <pipeline/test_support/test_operations.hpp>

#include <slideio/processor/core/error.hpp>
#include <slideio/processor/pipeline/operation_catalog.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>

namespace slideio::processor::testing_support {

namespace {

std::atomic<int64_t> gPredictCalls{0};

// A shift of the disks' values that depends on the tile.
uint64_t tileShift(const Rect64& tile) {
    const uint64_t x = static_cast<uint64_t>(tile.x);
    const uint64_t y = static_cast<uint64_t>(tile.y);
    return (x * 73856093u) ^ (y * 19349663u) ^ ((x + y) >> 3);
}

// The value of disk i in a tile of shift s: a rotation of 1..n, offset by a
// multiple of n, so values are distinct within the tile and differ between tiles.
int32_t valueOf(const DisksSpec& spec, size_t i, uint64_t s) {
    if (spec.sharedFirst >= 0 && spec.sharedSecond >= 0 && static_cast<size_t>(spec.sharedSecond) == i &&
        static_cast<size_t>(spec.sharedFirst) < spec.disks.size()) {
        i = static_cast<size_t>(spec.sharedFirst);
    }
    const uint64_t n = spec.disks.size();
    return static_cast<int32_t>(1 + (i + s) % n + n * (s % 5));
}

bool inside(const Disk& d, double px, double py) {
    const double dx = px - d.cx;
    const double dy = py - d.cy;
    const double d2 = dx * dx + dy * dy;
    return d2 <= d.radius * d.radius && (d.innerRadius <= 0.0 || d2 >= d.innerRadius * d.innerRadius);
}

} // namespace

OperationParameters disksParameters(const DisksSpec& spec) {
    std::vector<double> flat;
    for (const Disk& d : spec.disks) {
        flat.insert(flat.end(), {d.cx, d.cy, d.radius, d.innerRadius});
    }
    OperationParameters p;
    p.setInt("halo", spec.halo);
    p.setDoubleArray("disks", flat.data(), static_cast<int32_t>(flat.size()));
    p.setInt("sharedFirst", spec.sharedFirst);
    p.setInt("sharedSecond", spec.sharedSecond);
    return p;
}

DisksSpec disksSpecOf(const OperationParameters& parameters) {
    DisksSpec spec;
    int64_t value = 0;
    if (parameters.getInt("halo", value)) {
        if (value < 0) {
            throw InvalidArgumentError("test.disks: the halo must not be negative");
        }
        spec.halo = static_cast<int32_t>(value);
    }
    if (parameters.getInt("sharedFirst", value)) {
        spec.sharedFirst = static_cast<int32_t>(value);
    }
    if (parameters.getInt("sharedSecond", value)) {
        spec.sharedSecond = static_cast<int32_t>(value);
    }
    const int32_t count = parameters.getDoubleArray("disks", nullptr, 0);
    if (count % 4 != 0) {
        throw InvalidArgumentError("test.disks: 'disks' holds four numbers per disk");
    }
    std::vector<double> flat(static_cast<size_t>(count));
    parameters.getDoubleArray("disks", flat.data(), count);
    for (size_t i = 0; i + 3 < flat.size(); i += 4) {
        spec.disks.push_back(Disk{flat[i], flat[i + 1], flat[i + 2], flat[i + 3]});
    }
    return spec;
}

int32_t diskLabelAt(const DisksSpec& spec, const Rect64& tile, double levelScale, int64_t x, int64_t y) {
    const double px = (static_cast<double>(x) + 0.5) * levelScale;
    const double py = (static_cast<double>(y) + 0.5) * levelScale;
    int32_t label = 0;
    for (size_t i = 0; i < spec.disks.size(); ++i) {
        if (inside(spec.disks[i], px, py)) {
            label = valueOf(spec, i, tileShift(tile));
        }
    }
    return label;
}

DisksOperation::DisksOperation(const OperationParameters& parameters)
    : parameters_(parameters), spec_(disksSpecOf(parameters)) {}

const char* DisksOperation::name() const { return "test.disks"; }

Halo DisksOperation::halo() const { return Halo::uniform(spec_.halo); }

const OperationParameters& DisksOperation::parameters() const { return parameters_; }

void DisksOperation::predictLabels(const OperationContext& context, BlockView& labels) {
    ++gPredictCalls;
    const Rect64 halo = context.haloRectAtLevel();
    const Rect64 tile = context.tileRectAtLevel();
    const double scale = context.levelScale();
    const uint64_t shift = tileShift(tile);
    auto* base = static_cast<uint8_t*>(labels.planeData(0));
    const int64_t stride = labels.planeRowStrideBytes(0);
    // Each disk over its own bounding box, later disks over earlier ones, as
    // diskLabelAt() decides.
    for (size_t i = 0; i < spec_.disks.size(); ++i) {
        const Disk& d = spec_.disks[i];
        const int64_t x0 = std::max<int64_t>(halo.x, static_cast<int64_t>(std::floor((d.cx - d.radius) / scale)) - 1);
        const int64_t y0 = std::max<int64_t>(halo.y, static_cast<int64_t>(std::floor((d.cy - d.radius) / scale)) - 1);
        const int64_t x1 =
            std::min<int64_t>(halo.x + halo.width, static_cast<int64_t>(std::ceil((d.cx + d.radius) / scale)) + 1);
        const int64_t y1 =
            std::min<int64_t>(halo.y + halo.height, static_cast<int64_t>(std::ceil((d.cy + d.radius) / scale)) + 1);
        const int32_t value = valueOf(spec_, i, shift);
        for (int64_t y = y0; y < y1; ++y) {
            auto* row = base + (y - halo.y) * stride;
            for (int64_t x = x0; x < x1; ++x) {
                if (inside(d, (static_cast<double>(x) + 0.5) * scale, (static_cast<double>(y) + 0.5) * scale)) {
                    std::memcpy(row + (x - halo.x) * 4, &value, 4);
                }
            }
        }
    }
}

void registerDisksOperation() {
    OperationCatalog& catalog = OperationCatalog::instance();
    if (!catalog.isRegistered("test.disks")) {
        catalog.registerFactory(std::make_shared<PrototypeFactory>(
            [](const OperationParameters& p) -> OperationPtr { return std::make_shared<DisksOperation>(p); },
            "Analytic: filled disks and rings at fixed centres, labelled per tile",
            "O(n) in the pixels of the halo rect"));
    }
}

int64_t disksPredictCount() { return gPredictCalls.load(); }

} // namespace slideio::processor::testing_support
