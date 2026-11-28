// Requirements: UR-ALG-6, UR-IMG-10, UR-IMG-11, UR-QA-2
#pragma once

// The harness every algorithm test uses: apply an operation to one tile built in
// memory, or run it through the pipeline over a scene and assemble the level.

#include <slideio/processor/core/data_type.hpp>
#include <slideio/processor/core/flow.hpp>
#include <slideio/processor/core/geometry.hpp>
#include <slideio/processor/core/ids.hpp>
#include <slideio/processor/core/object_record.hpp>
#include <slideio/processor/pipeline/operation.hpp>
#include <slideio/processor/pipeline/pipeline_spec.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace slideio {
class Scene;
} // namespace slideio

namespace slideio::processor::testing_support {

/// A planar image: one tightly packed plane of width * height samples per type.
struct Image {
    int64_t width = 0;
    int64_t height = 0;
    std::vector<DataType> types;
    std::vector<std::vector<uint8_t>> planes;

    /// The sample at (x, y) of a plane as a double (exact for every type up to
    /// 2^53 in magnitude; Float16 decoded exactly).
    double at(int32_t plane, int64_t x, int64_t y) const;
};

/// Applies @p op to one width x height tile at level 0, as the framework would:
/// a PerChannel operation is applied once per plane on single-plane views, a
/// Joint one once on all planes. When the operation declares a halo, the input
/// covers the tile grown by it and the pixels beyond the tile read as zero, as
/// pixels beyond the level do in a run. Plane p has type
/// inputTypes[p] and source channel sourceChannels[p]; its samples are
/// pixel(p, x, y), cast to the type with saturation (NaN is 0 in an integer
/// type). Float16 is refused (std::invalid_argument): use applyDirectRaw. The
/// context reports @p pixelSizeMicrons (0: unknown) as the level-0 pixel size.
Image applyDirect(Operation& op, const std::vector<DataType>& inputTypes,
                  const std::vector<int32_t>& sourceChannels, int64_t width, int64_t height,
                  const std::function<double(int32_t plane, int64_t x, int64_t y)>& pixel,
                  double pixelSizeMicrons = 0.0);

/// One apply() call of a direct application, on a context already complete.
using ApplyCall = std::function<void()>;

/// As applyDirect, but every apply() call is made by @p around, which is given
/// the call and must make it once (to measure or observe it).
Image applyDirectAround(Operation& op, const std::vector<DataType>& inputTypes,
                        const std::vector<int32_t>& sourceChannels, int64_t width, int64_t height,
                        const std::function<double(int32_t plane, int64_t x, int64_t y)>& pixel,
                        const std::function<void(const ApplyCall& apply)>& around,
                        double pixelSizeMicrons = 0.0);

/// As applyDirect, but each plane is given as raw, tightly packed sample bytes
/// (width * height * dataTypeSizeBytes(type)), for inputs a double cannot carry
/// exactly: 64-bit integers beyond 2^53 and Float16.
Image applyDirectRaw(Operation& op, const std::vector<DataType>& inputTypes,
                     const std::vector<int32_t>& sourceChannels, int64_t width, int64_t height,
                     const std::vector<std::vector<uint8_t>>& rawPlanes, double pixelSizeMicrons = 0.0);

/// Runs Pipeline::run over @p scene with tile size tileSize x tileSize and
/// @p workers workers, and assembles the analysis level from the raster tiles.
/// With @p bindObjects, objectsLayer() is bound as "objects", so a chain ending
/// in an operation producing objects runs; its objects are discarded.
Image runEager(std::shared_ptr<slideio::Scene> scene, const PipelineSpec& spec, int32_t tileSize,
               int32_t workers, bool bindObjects = false);

/// The layer applyDirectObjects() emits into and runEagerObjects() binds as
/// "objects".
LayerId objectsLayer();

/// What one direct application of an object-producing operation emitted.
struct DirectObjects {
    /// Finished objects, as the run would store them: rings normalised, then
    /// passed through the operation's finishObject() (those it refuses are
    /// left out), in emission order.
    std::vector<ObjectRecord> objects;
    /// Seam fragments, level-0 polygons as emitted, in emission order.
    std::vector<Polygon> fragments;
    /// Each seam fragment's join key, parallel to fragments.
    std::vector<int64_t> fragmentKeys;
};

/// As applyDirect, for an operation producing objects: the context has an
/// object sink, objectsLayer() as its target layer and a collector of seam
/// fragments. The finish context reports @p pixelSizeMicrons and level 0.
DirectObjects applyDirectObjects(Operation& op, const std::vector<DataType>& inputTypes,
                                 const std::vector<int32_t>& sourceChannels, int64_t width, int64_t height,
                                 const std::function<double(int32_t plane, int64_t x, int64_t y)>& pixel,
                                 double pixelSizeMicrons = 0.0);

/// Runs Pipeline::runInto over @p scene with tile size tileSize x tileSize and
/// @p workers workers, with objectsLayer() bound as "objects", and returns the
/// stored objects sorted by id.
std::vector<ObjectRecord> runEagerObjects(std::shared_ptr<slideio::Scene> scene, const PipelineSpec& spec,
                                          int32_t tileSize, int32_t workers);

/// An Int64 (or, to test its refusal, UInt16) raster at level 0: value(x, y) at
/// each pixel, asked for pixels beyond the level too. @p value must be
/// thread-safe: the run's workers fill tiles concurrently.
class AnchorSource : public TileRasterSource {
public:
    explicit AnchorSource(std::function<int64_t(int64_t, int64_t)> value, DataType type = DataType::Int64);

    void fillTile(BlockView& out) override;
    DataType planeType() const override;
    int32_t planeCount() const override;

private:
    std::function<int64_t(int64_t, int64_t)> value_;
    DataType type_;
};

/// What one run gave: the assembled analysis level and the stored objects.
struct EagerRun {
    Image raster;
    std::vector<ObjectRecord> objects; // sorted by id
};

/// One Pipeline::runInto over @p scene with tile size tileSize x tileSize and
/// @p workers workers, with objectsLayer() bound as "objects" and, when given,
/// @p anchors bound as auxiliary raster "anchors": the raster runEager(...,
/// true) would assemble and the objects runEagerObjects would return, from the
/// same run.
EagerRun runEagerAll(std::shared_ptr<slideio::Scene> scene, const PipelineSpec& spec, int32_t tileSize,
                     int32_t workers, TileRasterSource* anchors = nullptr);

/// As runEagerObjects, with @p anchors bound as auxiliary raster "anchors".
std::vector<ObjectRecord> runWithAnchors(const std::shared_ptr<slideio::Scene>& scene, const PipelineSpec& spec,
                                         int32_t tileSize, TileRasterSource& anchors, int32_t workers = 4);

/// The first difference between @p objects and @p reference, compared object
/// by object: ids, layers, rings point for point and every measurement
/// exactly; empty when they are the same.
std::string objectsDifference(const std::vector<ObjectRecord>& reference, const std::vector<ObjectRecord>& objects);

/// Expects objectsDifference() to be empty, the failure prefixed by @p label.
void expectSameObjects(const std::vector<ObjectRecord>& reference, const std::vector<ObjectRecord>& objects,
                       const std::string& label);

} // namespace slideio::processor::testing_support
