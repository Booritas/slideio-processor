// Requirements: UR-EXE-2, UR-EXE-3, UR-EXE-4, UR-EXE-5
#pragma once

// What the executor tests instrument a run with: an operation that records how
// many tiles it is applied to at once and can fail on a chosen tile, and sinks
// that log their flush calls. Public headers only.

#include <slideio/processor/core/flow.hpp>
#include <slideio/processor/pipeline/operation.hpp>
#include <slideio/processor/pipeline/operation_parameters.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace slideio::processor::testing_support {

/// "test.probe": PerChannel, Byte and Float32, halo 0, output Float32 zeros.
/// Int parameters:
/// - "failX", "failY" (no default; both or no failure): the apply whose tile rect at the level holds
///   level pixel (failX, failY) throws, while failures are armed;
/// - "failKind" (default 0): what it throws: 0 std::runtime_error("deliberate
///   failure"), 1 IoError("deliberate failure"), 2 CancelledError("deliberate
///   cancellation");
/// - "sleepMs" (default 0): how long each apply sleeps, so concurrent applies overlap;
/// - "copy" (default 0): 1 writes the input tile as Float32 instead of zeros, so a
///   probe can sit in a chain without changing what follows.
/// Every apply is counted, and the number running at once is tracked. The counters
/// and the arming switch are process-wide: tests that use them run one at a time.
class ProbeOperation : public Operation {
public:
    explicit ProbeOperation(const OperationParameters& parameters);
    const char* name() const override;
    ChannelArity arity() const override;
    bool requiresUniformTypes() const override;
    DataTypeSet supportedTypes() const override;
    Halo halo() const override;
    DataType outputType(DataType inputType) const override;
    const OperationParameters& parameters() const override;
    void apply(OperationContext& context) override;

    /// Zeroes the apply count and the concurrency maximum.
    static void resetCounters();
    /// Applies so far, finished or not.
    static int64_t applyCount();
    /// The most applies ever running at once since resetCounters().
    static int32_t maxConcurrent();
    /// Arms (the default) or disarms every probe's failure.
    static void setFailuresArmed(bool armed);

private:
    OperationParameters parameters_;
    int64_t failX_ = -1;
    int64_t failY_ = -1;
    int64_t failKind_ = 0;
    int64_t sleepMs_ = 0;
    int64_t copy_ = 0;
    bool fails_ = false;  ///< Both failX and failY given.
};

/// Registers "test.probe", unless already registered.
void registerProbeOperation();

/// Where the logging sinks record their flush calls, in call order. Thread-safe.
class FlushLog {
public:
    void record(const char* sink);
    std::vector<std::string> entries() const;

private:
    mutable std::mutex mutex_;
    std::vector<std::string> entries_;
};

/// An ObjectSink that counts emitted records and logs "object" on flush.
class LoggingObjectSink : public ObjectSink {
public:
    explicit LoggingObjectSink(FlushLog& log);
    void emit(const ObjectRecord& record) override;
    void flush() override;
    int64_t emittedCount() const override;

private:
    FlushLog& log_;
    std::atomic<int64_t> emitted_{0};
};

/// An EdgeSink that counts assertions and logs "edge" on flush.
class LoggingEdgeSink : public EdgeSink {
public:
    explicit LoggingEdgeSink(FlushLog& log);
    void assertParent(ObjectId child, ObjectId parent) override;
    void assertDerivedFrom(ObjectId derived, ObjectId primitive) override;
    void flush() override;
    int64_t assertedCount() const override;

private:
    FlushLog& log_;
    std::atomic<int64_t> asserted_{0};
};

/// An AnnotationSink that counts assertions and logs "annotation" on flush.
class LoggingAnnotationSink : public AnnotationSink {
public:
    explicit LoggingAnnotationSink(FlushLog& log);
    void assertLabel(ObjectId object, LabelId label) override;
    void assertMeasurement(ObjectId object, const char* name, double value, MeasurementUnit unit) override;
    void flush() override;
    int64_t assertedCount() const override;

private:
    FlushLog& log_;
    std::atomic<int64_t> asserted_{0};
};

/// A RasterSink that counts tiles and logs "raster" on flush.
class LoggingRasterSink : public RasterSink {
public:
    explicit LoggingRasterSink(FlushLog& log);
    void writeTile(const BlockView& tile) override;
    void flush() override;
    int64_t tilesWritten() const override;

private:
    FlushLog& log_;
    std::atomic<int64_t> tiles_{0};
};

} // namespace slideio::processor::testing_support
