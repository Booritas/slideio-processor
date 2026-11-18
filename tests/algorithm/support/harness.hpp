// Requirements: UR-ALG-6, UR-IMG-10, UR-IMG-11, UR-QA-2
#pragma once

// The harness every algorithm test uses: apply an operation to one tile built in
// memory, or run it through the pipeline over a scene and assemble the level.

#include <slideio/processor/core/data_type.hpp>
#include <slideio/processor/pipeline/operation.hpp>
#include <slideio/processor/pipeline/pipeline_spec.hpp>

#include <cstdint>
#include <functional>
#include <memory>
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
/// type). Float16 is refused (std::invalid_argument): use applyDirectRaw.
Image applyDirect(Operation& op, const std::vector<DataType>& inputTypes,
                  const std::vector<int32_t>& sourceChannels, int64_t width, int64_t height,
                  const std::function<double(int32_t plane, int64_t x, int64_t y)>& pixel);

/// As applyDirect, but each plane is given as raw, tightly packed sample bytes
/// (width * height * dataTypeSizeBytes(type)), for inputs a double cannot carry
/// exactly: 64-bit integers beyond 2^53 and Float16.
Image applyDirectRaw(Operation& op, const std::vector<DataType>& inputTypes,
                     const std::vector<int32_t>& sourceChannels, int64_t width, int64_t height,
                     const std::vector<std::vector<uint8_t>>& rawPlanes);

/// Runs Pipeline::run over @p scene with tile size tileSize x tileSize and
/// @p workers workers, and assembles the analysis level from the raster tiles.
Image runEager(std::shared_ptr<slideio::Scene> scene, const PipelineSpec& spec, int32_t tileSize,
               int32_t workers);

} // namespace slideio::processor::testing_support
