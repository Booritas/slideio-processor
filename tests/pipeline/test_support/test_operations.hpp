// Requirements: UR-EXT-1, UR-EXT-2, UR-EXT-3, UR-SEG-3, UR-SEG-4
#pragma once

// Operations the pipeline tests run, written against the public headers only,
// exactly as an extension author would (UR-EXT-2). Plans B and C build on them.

#include <slideio/processor/core/flow.hpp>
#include <slideio/processor/core/geometry.hpp>
#include <slideio/processor/core/object_record.hpp>
#include <slideio/processor/pipeline/operation.hpp>
#include <slideio/processor/pipeline/operation_parameters.hpp>

#include <cstdint>
#include <mutex>
#include <vector>

namespace slideio::processor::testing_support {

/// A ring with collinear vertices removed (the wrap-around included), rotated to
/// start at its topmost-then-leftmost vertex; orientation is kept. The form in
/// which ComponentsOperation emits rings, and through which merged output is compared.
std::vector<Point64> normaliseRing(const std::vector<Point64>& ring);

/// An ObjectSink that keeps every record it is given, in emission order. Thread-safe.
class CollectingObjectSink : public ObjectSink {
public:
    void emit(const ObjectRecord& record) override;
    void flush() override;
    int64_t emittedCount() const override;
    /// A copy of the records emitted so far.
    std::vector<ObjectRecord> records() const;

private:
    mutable std::mutex mutex_;
    std::vector<ObjectRecord> records_;
};

/// "test.threshold": PerChannel, Byte and Float32, halo 0, output Byte: 255 where the
/// sample is at least parameter "level" (double or int, default 128), else 0.
class ThresholdOperation : public Operation {
public:
    explicit ThresholdOperation(const OperationParameters& parameters);
    const char* name() const override;
    ChannelArity arity() const override;
    bool requiresUniformTypes() const override;
    DataTypeSet supportedTypes() const override;
    Halo halo() const override;
    DataType outputType(DataType inputType) const override;
    const OperationParameters& parameters() const override;
    void apply(OperationContext& context) override;

private:
    OperationParameters parameters_;
    double level_;
};

/// "test.boxMean": PerChannel, Byte, UInt16 and Float32, halo r from int parameter
/// "radius" (default 1, negative refused), output Float32: the mean over the
/// (2r+1)^2 window centred on each tile pixel.
class BoxMeanOperation : public Operation {
public:
    explicit BoxMeanOperation(const OperationParameters& parameters);
    const char* name() const override;
    ChannelArity arity() const override;
    bool requiresUniformTypes() const override;
    DataTypeSet supportedTypes() const override;
    Halo halo() const override;
    DataType outputType(DataType inputType) const override;
    const OperationParameters& parameters() const override;
    void apply(OperationContext& context) override;

private:
    OperationParameters parameters_;
    int32_t radius_;
};

/// "test.components": Joint, Byte only, one input plane, halo 0, produces objects;
/// the raster output is a copy of the input's tile. Components are found in the
/// tile rect only, never in the halo around it, so each is anchored in the tile, and
/// one continuing past the tile edge is its tile-clipped piece. Emits one
/// ObjectRecord per 4-connected non-zero component: a polygon of pixel-edge rings in level-0 coordinates (through
/// OperationContext::toLevel0), each ring normalised by normaliseRing, outer ring
/// first and clockwise on screen, holes after it ordered by first vertex. It is
/// measured with applyStandardMeasurements from one IntensityPartial of the
/// component's samples (channel = the plane's source channel). Components are
/// emitted in raster order of their first pixel. When bool parameter "seams" is
/// true (default false), a component touching the tile rect's edge goes to
/// emitSeamFragment with that partial instead. A plain flood fill, no library call.
class ComponentsOperation : public Operation {
public:
    explicit ComponentsOperation(const OperationParameters& parameters);
    const char* name() const override;
    ChannelArity arity() const override;
    bool requiresUniformTypes() const override;
    DataTypeSet supportedTypes() const override;
    Halo halo() const override;
    DataType outputType(DataType inputType) const override;
    const OperationParameters& parameters() const override;
    void apply(OperationContext& context) override;
    bool producesObjects() const override;

private:
    OperationParameters parameters_;
    bool seams_;
};

} // namespace slideio::processor::testing_support
