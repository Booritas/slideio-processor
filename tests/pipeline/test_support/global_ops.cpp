// Requirements: UR-PIX-5, UR-QA-3, UR-EXE-1, UR-EXT-4
#include <pipeline/test_support/global_ops.hpp>

#include <pipeline/test_support/test_operations.hpp>

#include <slideio/processor/core/error.hpp>
#include <slideio/processor/pipeline/operation_catalog.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>

namespace slideio::processor::testing_support {

namespace {

double sampleAt(const BlockView& view, int32_t plane, int64_t x, int64_t y) {
    const auto* row = static_cast<const unsigned char*>(view.planeData(plane)) + y * view.planeRowStrideBytes(plane);
    switch (view.planeType(plane)) {
    case DataType::Byte:
        return row[x];
    case DataType::Float32: {
        float v;
        std::memcpy(&v, row + x * 4, sizeof v);
        return v;
    }
    default:
        throw InvalidArgumentError("test global operation: unsupported plane type");
    }
}

// The tile rect's part of the input, which spans the halo rect, as a view of its own.
BlockView tileView(const OperationContext& context) {
    const BlockView& input = context.input();
    const Rect64 tile = context.tileRectAtLevel();
    const Rect64 halo = context.haloRectAtLevel();
    std::vector<BlockPlane> planes;
    for (int32_t plane = 0; plane < input.planeCount(); ++plane) {
        const int64_t size = dataTypeSizeBytes(input.planeType(plane));
        auto* first = static_cast<unsigned char*>(input.planeData(plane)) +
                      (tile.y - halo.y) * input.planeRowStrideBytes(plane) + (tile.x - halo.x) * size;
        planes.push_back(
            BlockPlane{first, input.planeType(plane), input.planeRowStrideBytes(plane), input.planeSourceChannel(plane)});
    }
    return BlockView(tile.width, tile.height, context.tileRect(), context.level(), planes.data(),
                     static_cast<int32_t>(planes.size()));
}

template <typename T>
const T& sameKind(const Operation& self, const Operation& other) {
    const auto* typed = dynamic_cast<const T*>(&other);
    if (typed == nullptr) {
        throw InvalidArgumentError((std::string(self.name()) + ": combine() was given another kind of operation").c_str());
    }
    return *typed;
}

// The accumulate() calls finished on tiles other than (0, 0) since resetHold(), for "holdFirstTile".
std::mutex gHoldMutex;
std::condition_variable gHoldChanged;
int64_t gOthersDone = 0;

std::atomic<int32_t> gLastStatePlaneCount{0};

} // namespace

// --- FloatSumOperation --------------------------------------------------------------

FloatSumOperation::FloatSumOperation(const OperationParameters& parameters) : parameters_(parameters) {
    int64_t offset = 1;
    parameters_.getInt("offset", offset);
    if (offset != 0 && offset != 1) {
        throw InvalidArgumentError("test.floatSum: offset must be 0 or 1");
    }
    offset_ = offset == 1;
    parameters_.getInt("holdFirstTile", holdFirstTile_);
    if (holdFirstTile_ < 0) {
        throw InvalidArgumentError("test.floatSum: holdFirstTile must not be negative");
    }
    int64_t failFinalize = 0;
    parameters_.getInt("failFinalize", failFinalize);
    failFinalize_ = failFinalize != 0;
    parameters_.getInt("stateBytes", stateBytes_);
}

int64_t FloatSumOperation::accumulatedStateBytes(int32_t planeCount) const {
    gLastStatePlaneCount.store(planeCount);
    return stateBytes_;
}

int32_t FloatSumOperation::lastStatePlaneCount() { return gLastStatePlaneCount.load(); }
void FloatSumOperation::resetStatePlaneCount() { gLastStatePlaneCount.store(0); }

const char* FloatSumOperation::name() const { return "test.floatSum"; }
ChannelArity FloatSumOperation::arity() const { return ChannelArity::PerChannel; }
bool FloatSumOperation::requiresUniformTypes() const { return false; }
DataTypeSet FloatSumOperation::supportedTypes() const {
    return DataTypeSet().add(DataType::Byte).add(DataType::Float32);
}
Halo FloatSumOperation::halo() const { return Halo::none(); }
DataType FloatSumOperation::outputType(DataType) const { return DataType::Float32; }
const OperationParameters& FloatSumOperation::parameters() const { return parameters_; }
bool FloatSumOperation::needsGlobalContext() const { return true; }

double FloatSumOperation::tilePartial(const Rect64& tileRectAtLevel, double sampleSum, bool offset) {
    if (!offset) {
        return sampleSum;
    }
    const bool first = tileRectAtLevel.x == 0 && tileRectAtLevel.y == 0;
    return sampleSum + (first ? 9007199254740992.0 : 0.0);  // 2^53
}

void FloatSumOperation::resetHold() {
    std::lock_guard<std::mutex> lock(gHoldMutex);
    gOthersDone = 0;
}

void FloatSumOperation::accumulate(const OperationContext& context) {
    const Rect64 tile = context.tileRectAtLevel();
    const bool first = tile.x == 0 && tile.y == 0;
    if (first && holdFirstTile_ > 0) {
        std::unique_lock<std::mutex> lock(gHoldMutex);
        if (!gHoldChanged.wait_for(lock, std::chrono::seconds(10), [this] { return gOthersDone >= holdFirstTile_; })) {
            throw std::runtime_error("test.floatSum: the held first tile timed out");
        }
    }
    const BlockView view = tileView(context);
    double sampleSum = 0.0;
    for (int32_t plane = 0; plane < view.planeCount(); ++plane) {
        for (int64_t y = 0; y < view.height(); ++y) {
            for (int64_t x = 0; x < view.width(); ++x) {
                sampleSum += sampleAt(view, plane, x, y);
            }
        }
    }
    sum_ += tilePartial(context.tileRectAtLevel(), sampleSum, offset_);
    count_ += view.width() * view.height() * view.planeCount();
    if (!first) {
        {
            std::lock_guard<std::mutex> lock(gHoldMutex);
            ++gOthersDone;
        }
        gHoldChanged.notify_all();
    }
}

void FloatSumOperation::combine(const Operation& other) {
    const FloatSumOperation& o = sameKind<FloatSumOperation>(*this, other);
    sum_ += o.sum_;
    count_ += o.count_;
}

void FloatSumOperation::finalizeAccumulation() {
    if (failFinalize_) {
        throw IoError("deliberate failure");
    }
    mean_ = count_ > 0 ? sum_ / static_cast<double>(count_) : 0.0;
    finalised_ = true;
}

void FloatSumOperation::apply(OperationContext& context) {
    if (!finalised_) {
        throw InternalError("test.floatSum: apply() before finalizeAccumulation()");
    }
    const BlockView in = tileView(context);
    BlockView& out = context.output();
    for (int32_t plane = 0; plane < out.planeCount(); ++plane) {
        for (int64_t y = 0; y < out.height(); ++y) {
            auto* row = static_cast<unsigned char*>(out.planeData(plane)) + y * out.planeRowStrideBytes(plane);
            for (int64_t x = 0; x < out.width(); ++x) {
                const auto v = static_cast<float>(sampleAt(in, plane, x, y) - mean_);
                std::memcpy(row + x * 4, &v, sizeof v);
            }
        }
    }
}

double FloatSumOperation::sum() const { return sum_; }
int64_t FloatSumOperation::count() const { return count_; }
bool FloatSumOperation::finalised() const { return finalised_; }

// --- OtsuOperation ------------------------------------------------------------------

OtsuOperation::OtsuOperation(const OperationParameters& parameters)
    : parameters_(parameters), histogram_(256, 0.0, 256.0) {}

const char* OtsuOperation::name() const { return "test.otsu"; }
ChannelArity OtsuOperation::arity() const { return ChannelArity::PerChannel; }
bool OtsuOperation::requiresUniformTypes() const { return false; }
DataTypeSet OtsuOperation::supportedTypes() const { return DataTypeSet().add(DataType::Byte); }
Halo OtsuOperation::halo() const { return Halo::none(); }
DataType OtsuOperation::outputType(DataType) const { return DataType::Byte; }
const OperationParameters& OtsuOperation::parameters() const { return parameters_; }
bool OtsuOperation::needsGlobalContext() const { return true; }

void OtsuOperation::accumulate(const OperationContext& context) { histogram_.accumulate(tileView(context), 0); }

void OtsuOperation::combine(const Operation& other) {
    histogram_.combine(sameKind<OtsuOperation>(*this, other).histogram_);
}

void OtsuOperation::finalizeAccumulation() {
    std::vector<int64_t> bins(256);
    for (int32_t i = 0; i < 256; ++i) {
        bins[static_cast<size_t>(i)] = histogram_.bin(i);
    }
    threshold_ = otsuThreshold(bins);
}

void OtsuOperation::apply(OperationContext& context) {
    if (threshold_ < 0) {
        throw InternalError("test.otsu: apply() before finalizeAccumulation()");
    }
    const BlockView in = tileView(context);
    BlockView& out = context.output();
    for (int32_t plane = 0; plane < out.planeCount(); ++plane) {
        for (int64_t y = 0; y < out.height(); ++y) {
            auto* row = static_cast<unsigned char*>(out.planeData(plane)) + y * out.planeRowStrideBytes(plane);
            for (int64_t x = 0; x < out.width(); ++x) {
                row[x] = sampleAt(in, plane, x, y) > threshold_ ? 255 : 0;
            }
        }
    }
}

int32_t OtsuOperation::threshold() const { return threshold_; }
const Histogram64& OtsuOperation::histogram() const { return histogram_; }

int32_t OtsuOperation::otsuThreshold(const std::vector<int64_t>& bins) {
    double total = 0.0;
    double weighted = 0.0;
    for (size_t i = 0; i < bins.size(); ++i) {
        total += static_cast<double>(bins[i]);
        weighted += static_cast<double>(i) * static_cast<double>(bins[i]);
    }
    int32_t best = 0;
    double bestVariance = -1.0;
    double below = 0.0;
    double belowWeighted = 0.0;
    for (size_t t = 0; t + 1 < bins.size(); ++t) {
        below += static_cast<double>(bins[t]);
        belowWeighted += static_cast<double>(t) * static_cast<double>(bins[t]);
        const double above = total - below;
        if (below == 0.0 || above == 0.0) {
            continue;
        }
        const double meanBelow = belowWeighted / below;
        const double meanAbove = (weighted - belowWeighted) / above;
        const double variance = below * above * (meanBelow - meanAbove) * (meanBelow - meanAbove);
        if (variance > bestVariance) {
            bestVariance = variance;
            best = static_cast<int32_t>(t);
        }
    }
    return best;
}

// --- Registration -------------------------------------------------------------------

void registerGlobalOperations() {
    OperationCatalog& catalog = OperationCatalog::instance();
    const OperationFactoryPtr factories[] = {
        std::make_shared<PrototypeFactory>(
            [](const OperationParameters& p) -> OperationPtr { return std::make_shared<FloatSumOperation>(p); },
            "Definition: the sum of the samples, combined in canonical tile order", "O(n) in the number of samples"),
        std::make_shared<PrototypeFactory>(
            [](const OperationParameters& p) -> OperationPtr { return std::make_shared<OtsuOperation>(p); },
            "Otsu (1979): the threshold maximising the between-class variance of a 256-bin histogram",
            "O(n) in the number of samples"),
    };
    for (const OperationFactoryPtr& factory : factories) {
        if (!catalog.isRegistered(factory->name())) {
            catalog.registerFactory(factory);
        }
    }
}

} // namespace slideio::processor::testing_support
