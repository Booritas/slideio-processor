// Requirements: UR-EXT-1, UR-EXT-2, UR-EXT-3, UR-SEG-3, UR-SEG-4
#pragma once

// Operations the pipeline tests run, written against the public headers only,
// exactly as an extension author would (UR-EXT-2). Plans B and C build on them.
// The one exception is normaliseRing, which delegates to the engine's
// detail::normaliseRing so that fragments and merged objects share one form.

#include <slideio/processor/core/flow.hpp>
#include <slideio/processor/core/geometry.hpp>
#include <slideio/processor/core/memory_budget.hpp>
#include <slideio/processor/core/object_record.hpp>
#include <slideio/processor/pipeline/operation.hpp>
#include <slideio/processor/pipeline/operation_catalog.hpp>
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
/// (2r+1)^2 window centred on each tile pixel. Int parameter "declaredHalo"
/// (default: the radius) overrides the declared halo without changing the window,
/// to show what an under-declared halo does; window samples outside the input
/// then count as 0.
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
    int32_t declaredHalo_;
};

/// "test.components": Joint, halo 0, produces objects, in one of two forms:
/// - one plane, the Byte mask, which is also the intensity measured;
/// - two planes, plane 0 the Byte mask and plane 1 the intensity, Byte or
///   Float32, from which the partials are taken (how a real segmenter measures).
/// supportedTypes() is therefore {Byte, Float32} with mixed types allowed, but
/// Float32 is accepted only as the intensity plane of the two-plane form. The
/// raster output is one Byte plane, a copy of the mask's tile. Components are
/// found in the tile rect only, never in the halo around it, so each is anchored
/// in the tile, and one continuing past the tile edge is its tile-clipped piece.
/// Emits one ObjectRecord per 4-connected non-zero component of the mask: a
/// polygon of pixel-edge rings in level-0 coordinates (through
/// OperationContext::toLevel0), each ring normalised by normaliseRing, outer ring
/// first and clockwise on screen, holes after it ordered by first vertex (y, then
/// x). It is measured with applyStandardMeasurements from one IntensityPartial of
/// the component's intensity samples (channel = the intensity plane's source
/// channel). Components are emitted in raster order of their first pixel. When
/// bool parameter "seams" is true (default false), a component touching the tile
/// rect's edge goes to emitSeamFragment with that partial instead. A plain flood
/// fill, no library call.
/// Int parameter "halo" (default 0, in [0, 1024]) sets the declared uniform halo.
/// When bool parameter "haloScan" is true (default false), components are found
/// in the whole input (the halo rect) instead of the tile rect, and every one is
/// emitted, wherever its anchor lies: the framework's anchor-ownership rule keeps
/// each object once.
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
    /// 1 for either form: the mask copy.
    int32_t outputPlaneCount(int32_t inputPlaneCount) const override;

private:
    OperationParameters parameters_;
    bool seams_;
    bool haloScan_;
    int32_t halo_;
};

/// "test.wholeLevel": PerChannel, Byte and Float32, halo 0, output Float32, a
/// WholeLevel operation with a working set of 100 bytes per level pixel. Its tile
/// is the whole level; it writes each sample of the tile as Float32.
class WholeLevelOperation : public Operation {
public:
    explicit WholeLevelOperation(const OperationParameters& parameters);
    const char* name() const override;
    ChannelArity arity() const override;
    bool requiresUniformTypes() const override;
    DataTypeSet supportedTypes() const override;
    Halo halo() const override;
    DataType outputType(DataType inputType) const override;
    const OperationParameters& parameters() const override;
    void apply(OperationContext& context) override;
    TileRequirement tileRequirement() const override;
    int64_t workingSetBytesPerLevelPixel() const override;

private:
    OperationParameters parameters_;
};

/// "test.jointUniform": Joint, requiresUniformTypes, Byte and Float32, halo 0,
/// output one plane of the input type: a copy of plane 0's tile.
class JointUniformOperation : public Operation {
public:
    explicit JointUniformOperation(const OperationParameters& parameters);
    const char* name() const override;
    ChannelArity arity() const override;
    bool requiresUniformTypes() const override;
    DataTypeSet supportedTypes() const override;
    Halo halo() const override;
    DataType outputType(DataType inputType) const override;
    const OperationParameters& parameters() const override;
    void apply(OperationContext& context) override;
    /// 1: the copy of plane 0.
    int32_t outputPlaneCount(int32_t inputPlaneCount) const override;

private:
    OperationParameters parameters_;
};

/// "test.floatOnly": PerChannel, Float32 only, halo 0, output Float32: a copy of
/// the tile.
class FloatOnlyOperation : public Operation {
public:
    explicit FloatOnlyOperation(const OperationParameters& parameters);
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
};

/// "test.workingSet": PerChannel, Byte and Float32, halo 0, output Float32: a copy
/// of the tile as Float32. It declares a working set from its int parameters
/// "perPixel" (default 2) and "perTile" (default 512), unchecked, so a test can
/// declare any figure, negative ones included. It allocates nothing beyond them.
/// While a budget is observed (observe()), each apply() records the budget's
/// reservedBytes(), so a test sees what the run reserved for the tile.
class WorkingSetOperation : public Operation {
public:
    explicit WorkingSetOperation(const OperationParameters& parameters);
    const char* name() const override;
    ChannelArity arity() const override;
    bool requiresUniformTypes() const override;
    DataTypeSet supportedTypes() const override;
    Halo halo() const override;
    DataType outputType(DataType inputType) const override;
    const OperationParameters& parameters() const override;
    void apply(OperationContext& context) override;
    int64_t workingSetBytesPerLevelPixel() const override;
    int64_t workingSetBytesPerTile() const override;

    /// Starts recording the largest reservedBytes() of @p budget seen by any
    /// apply(), from 0; null stops recording. The budget must outlive the recording.
    static void observe(const MemoryBudget* budget);
    /// The largest reservedBytes() seen since observe().
    static int64_t observedReservedBytes();

private:
    OperationParameters parameters_;
    int64_t perPixel_;
    int64_t perTile_;
};

/// A factory whose declarations are read from a prototype operation built from
/// default parameters, so they describe the operation create() returns for
/// default parameters. The catalog's create() compares only the
/// parameter-independent declarations (name, arity, requiresUniformTypes,
/// supportedTypes, producesObjects), so a parameter that changes the halo, such
/// as the BoxMean radius, is accepted.
class PrototypeFactory : public OperationFactory {
public:
    using Maker = OperationPtr (*)(const OperationParameters&);
    PrototypeFactory(Maker maker, const char* correctnessReference, const char* complexityNote);
    OperationPtr create(const OperationParameters& parameters) const override;
    const char* name() const override;
    ChannelArity arity() const override;
    bool requiresUniformTypes() const override;
    DataTypeSet supportedTypes() const override;
    Halo halo() const override;
    const char* correctnessReference() const override;
    const char* complexityNote() const override;
    TileRequirement tileRequirement() const override;
    int64_t workingSetBytesPerLevelPixel() const override;
    bool producesObjects() const override;
    int32_t outputPlaneCount(int32_t inputPlaneCount) const override;
    int64_t workingSetBytesPerTile() const override;
    int64_t accumulatedStateBytes(int32_t planeCount) const override;

private:
    Maker maker_;
    OperationPtr prototype_;
    const char* correctnessReference_;
    const char* complexityNote_;
};

/// Factories of the test operations. The catalog is a process singleton, so a
/// test registers them through registerTestOperations().
OperationFactoryPtr makeThresholdFactory();
OperationFactoryPtr makeBoxMeanFactory();
OperationFactoryPtr makeComponentsFactory();
OperationFactoryPtr makeWholeLevelFactory();
OperationFactoryPtr makeJointUniformFactory();
OperationFactoryPtr makeFloatOnlyFactory();
OperationFactoryPtr makeWorkingSetFactory();
/// Registers the seven factories above, skipping any already registered.
void registerTestOperations();

} // namespace slideio::processor::testing_support
