// Requirements: UR-PIX-5, UR-QA-3, UR-EXE-1, UR-EXT-4
#pragma once

// Global-context operations the pipeline tests run, written against the public
// headers only, as an extension author would (UR-EXT-2).

#include <slideio/processor/core/geometry.hpp>
#include <slideio/processor/pipeline/operation.hpp>
#include <slideio/processor/pipeline/operation_parameters.hpp>
#include <slideio/processor/pipeline/reduction.hpp>

#include <cstdint>
#include <vector>

namespace slideio::processor::testing_support {

/// "test.floatSum": PerChannel, Byte and Float32, halo 0, output Float32, global
/// context. accumulate() adds the samples of the tile rect into a double, then adds
/// tilePartial()'s offset: 2^53 for the tile whose rect starts at level pixel
/// (0, 0), nothing for any other, when int parameter "offset" is 1 (the default;
/// 0 adds none). combine() adds the other's sum and count. With the offset, a small
/// partial combined after the first tile's is rounded at the 2^53 scale (ulp 2),
/// while partials combined before it add exactly: the finalised sum depends on the
/// order of combination, a deliberately order-sensitive accumulator. When every
/// tile's samples sum to 1, ascending order gives exactly 2^53, and any order that
/// combines two or more other tiles before the first gives at least 2^53 + 2.
/// finalizeAccumulation() sets the mean, sum / count; apply() writes sample − mean
/// as Float32.
///
/// Test hooks (int parameters):
/// - "holdFirstTile" (default 0): accumulate() on the tile at (0, 0) waits until
///   that many accumulate() calls on other tiles have finished since resetHold(),
///   so the first tile's partial arrives after theirs; it throws
///   std::runtime_error after 10 s rather than hang. Process-wide state: tests
///   that use it run one at a time.
/// - "failFinalize" (default 0): 1 makes finalizeAccumulation() throw
///   IoError("deliberate failure").
/// - "stateBytes" (default 0): what accumulatedStateBytes() returns, unchecked, so a
///   test can declare any figure, negative ones included. The plane count of its
///   last call is kept in lastStatePlaneCount().
class FloatSumOperation : public Operation {
public:
    explicit FloatSumOperation(const OperationParameters& parameters);
    const char* name() const override;
    ChannelArity arity() const override;
    bool requiresUniformTypes() const override;
    DataTypeSet supportedTypes() const override;
    Halo halo() const override;
    DataType outputType(DataType inputType) const override;
    const OperationParameters& parameters() const override;
    void apply(OperationContext& context) override;
    bool needsGlobalContext() const override;
    void accumulate(const OperationContext& context) override;
    void combine(const Operation& other) override;
    void finalizeAccumulation() override;
    int64_t accumulatedStateBytes(int32_t planeCount) const override;

    /// The planeCount of the last accumulatedStateBytes() call on any instance; 0 after resetStatePlaneCount().
    static int32_t lastStatePlaneCount();
    static void resetStatePlaneCount();

    /// The accumulated sum, offsets included.
    double sum() const;
    /// The samples accumulated.
    int64_t count() const;
    /// Whether finalizeAccumulation() has run.
    bool finalised() const;

    /// What accumulate() adds for one tile whose samples sum to `sampleSum`.
    static double tilePartial(const Rect64& tileRectAtLevel, double sampleSum, bool offset);

    /// Zeroes the count of finished accumulate() calls that "holdFirstTile" waits on.
    static void resetHold();

private:
    OperationParameters parameters_;
    bool offset_ = true;
    int64_t holdFirstTile_ = 0;
    bool failFinalize_ = false;
    int64_t stateBytes_ = 0;
    double sum_ = 0.0;
    int64_t count_ = 0;
    double mean_ = 0.0;
    bool finalised_ = false;
};

/// "test.otsu": PerChannel, Byte, halo 0, output Byte, global context.
/// accumulate() adds the tile rect's samples to a Histogram64(256, 0, 256);
/// combine() adds histograms; finalizeAccumulation() sets the Otsu threshold of
/// the histogram (otsuThreshold); apply() writes 255 where the sample is above the
/// threshold, else 0.
class OtsuOperation : public Operation {
public:
    explicit OtsuOperation(const OperationParameters& parameters);
    const char* name() const override;
    ChannelArity arity() const override;
    bool requiresUniformTypes() const override;
    DataTypeSet supportedTypes() const override;
    Halo halo() const override;
    DataType outputType(DataType inputType) const override;
    const OperationParameters& parameters() const override;
    void apply(OperationContext& context) override;
    bool needsGlobalContext() const override;
    void accumulate(const OperationContext& context) override;
    void combine(const Operation& other) override;
    void finalizeAccumulation() override;

    /// The finalised threshold; -1 before finalizeAccumulation().
    int32_t threshold() const;
    /// The accumulated histogram.
    const Histogram64& histogram() const;

    /// The Otsu threshold of `bins`: the t maximising the between-class variance of
    /// {bins <= t} and {bins > t}, the lowest such t; 0 when the histogram is empty.
    static int32_t otsuThreshold(const std::vector<int64_t>& bins);

private:
    OperationParameters parameters_;
    Histogram64 histogram_;
    int32_t threshold_ = -1;
};

/// Registers "test.floatSum" and "test.otsu", skipping any already registered.
void registerGlobalOperations();

} // namespace slideio::processor::testing_support
