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
/// tilePartial()'s offset: +1e16 or -1e16 by the parity of the 64-pixel checkerboard
/// cell holding the tile's top-left level pixel, when int parameter "offset" is 1
/// (the default; 0 adds none). combine() adds the other's sum and count, so with
/// offsets the finalised sum depends on the order in which partials are combined:
/// a deliberately order-sensitive accumulator. finalizeAccumulation() sets the mean,
/// sum / count; apply() writes sample − mean as Float32.
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

    /// The accumulated sum, offsets included.
    double sum() const;
    /// The samples accumulated.
    int64_t count() const;
    /// Whether finalizeAccumulation() has run.
    bool finalised() const;

    /// What accumulate() adds for one tile whose samples sum to `sampleSum`.
    static double tilePartial(const Rect64& tileRectAtLevel, double sampleSum, bool offset);

private:
    OperationParameters parameters_;
    bool offset_ = true;
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
