// Requirements: UR-EXE-2, UR-EXE-3, UR-EXE-4, UR-EXE-5
#include <pipeline/test_support/executor_support.hpp>

#include <pipeline/test_support/test_operations.hpp>

#include <slideio/processor/core/error.hpp>
#include <slideio/processor/pipeline/operation_catalog.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <thread>

namespace slideio::processor::testing_support {

namespace {

std::atomic<int64_t> gApplies{0};
std::atomic<int32_t> gRunning{0};
std::atomic<int32_t> gMaxRunning{0};
std::atomic<bool> gArmed{true};

// Counts an apply as running for its lifetime.
class RunningScope {
public:
    RunningScope() {
        ++gApplies;
        const int32_t now = ++gRunning;
        int32_t seen = gMaxRunning.load();
        while (now > seen && !gMaxRunning.compare_exchange_weak(seen, now)) {
        }
    }
    ~RunningScope() { --gRunning; }
    RunningScope(const RunningScope&) = delete;
    RunningScope& operator=(const RunningScope&) = delete;
};

} // namespace

// --- ProbeOperation -----------------------------------------------------------------

ProbeOperation::ProbeOperation(const OperationParameters& parameters) : parameters_(parameters) {
    parameters_.getInt("failX", failX_);
    parameters_.getInt("failY", failY_);
    parameters_.getInt("failKind", failKind_);
    parameters_.getInt("sleepMs", sleepMs_);
    if (failKind_ < 0 || failKind_ > 2) {
        throw InvalidArgumentError("test.probe: failKind must be 0, 1 or 2");
    }
    if (sleepMs_ < 0 || sleepMs_ > 1000) {
        throw InvalidArgumentError("test.probe: sleepMs must be in [0, 1000]");
    }
}

const char* ProbeOperation::name() const { return "test.probe"; }
ChannelArity ProbeOperation::arity() const { return ChannelArity::PerChannel; }
bool ProbeOperation::requiresUniformTypes() const { return false; }
DataTypeSet ProbeOperation::supportedTypes() const { return DataTypeSet().add(DataType::Byte).add(DataType::Float32); }
Halo ProbeOperation::halo() const { return Halo::none(); }
DataType ProbeOperation::outputType(DataType) const { return DataType::Float32; }
const OperationParameters& ProbeOperation::parameters() const { return parameters_; }

void ProbeOperation::apply(OperationContext& context) {
    const RunningScope running;
    if (sleepMs_ > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs_));
    }
    const Rect64 tile = context.tileRectAtLevel();
    if (gArmed && failX_ >= tile.x && failX_ < tile.x + tile.width && failY_ >= tile.y &&
        failY_ < tile.y + tile.height) {
        switch (failKind_) {
        case 1:
            throw IoError("deliberate failure");
        case 2:
            throw CancelledError("deliberate cancellation");
        default:
            throw std::runtime_error("deliberate failure");
        }
    }
    BlockView& out = context.output();
    for (int32_t plane = 0; plane < out.planeCount(); ++plane) {
        for (int64_t y = 0; y < out.height(); ++y) {
            auto* row = static_cast<unsigned char*>(out.planeData(plane)) + y * out.planeRowStrideBytes(plane);
            std::memset(row, 0, static_cast<size_t>(out.width() * 4));
        }
    }
}

void ProbeOperation::resetCounters() {
    gApplies = 0;
    gMaxRunning = 0;
}
int64_t ProbeOperation::applyCount() { return gApplies.load(); }
int32_t ProbeOperation::maxConcurrent() { return gMaxRunning.load(); }
void ProbeOperation::setFailuresArmed(bool armed) { gArmed = armed; }

void registerProbeOperation() {
    OperationCatalog& catalog = OperationCatalog::instance();
    if (!catalog.isRegistered("test.probe")) {
        catalog.registerFactory(std::make_shared<PrototypeFactory>(
            [](const OperationParameters& p) -> OperationPtr { return std::make_shared<ProbeOperation>(p); },
            "Analytic: out = 0", "O(n) in the number of samples"));
    }
}

// --- Flush logging ------------------------------------------------------------------

void FlushLog::record(const char* sink) {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.emplace_back(sink);
}

std::vector<std::string> FlushLog::entries() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_;
}

LoggingObjectSink::LoggingObjectSink(FlushLog& log) : log_(log) {}
void LoggingObjectSink::emit(const ObjectRecord&) { ++emitted_; }
void LoggingObjectSink::flush() { log_.record("object"); }
int64_t LoggingObjectSink::emittedCount() const { return emitted_; }

LoggingEdgeSink::LoggingEdgeSink(FlushLog& log) : log_(log) {}
void LoggingEdgeSink::assertParent(ObjectId, ObjectId) { ++asserted_; }
void LoggingEdgeSink::assertDerivedFrom(ObjectId, ObjectId) { ++asserted_; }
void LoggingEdgeSink::flush() { log_.record("edge"); }
int64_t LoggingEdgeSink::assertedCount() const { return asserted_; }

LoggingAnnotationSink::LoggingAnnotationSink(FlushLog& log) : log_(log) {}
void LoggingAnnotationSink::assertLabel(ObjectId, LabelId) { ++asserted_; }
void LoggingAnnotationSink::assertMeasurement(ObjectId, const char*, double, MeasurementUnit) { ++asserted_; }
void LoggingAnnotationSink::flush() { log_.record("annotation"); }
int64_t LoggingAnnotationSink::assertedCount() const { return asserted_; }

LoggingRasterSink::LoggingRasterSink(FlushLog& log) : log_(log) {}
void LoggingRasterSink::writeTile(const BlockView&) { ++tiles_; }
void LoggingRasterSink::flush() { log_.record("raster"); }
int64_t LoggingRasterSink::tilesWritten() const { return tiles_; }

} // namespace slideio::processor::testing_support
