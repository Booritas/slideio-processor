// Requirements: UR-IMG-9, UR-IMG-4
#pragma once

#include <slideio/processor/core/block_view.hpp>
#include <slideio/processor/core/data_type.hpp>
#include <slideio/processor/core/geometry.hpp>

#include <cstdint>
#include <vector>

namespace slideio::processor::detail {

/// The owned memory behind one raster block (ruling P18): one zero-initialised
/// std::vector<uint8_t> per plane, row stride width x sample size. Not shared
/// between threads; the BlockView it produces points into it and is valid until
/// the buffers are destroyed or moved.
class PlaneBuffers {
public:
    /// One plane per entry of `types`, each holding the scene channel named at the
    /// same position of `sourceChannels`. Throws InvalidArgumentError for a
    /// non-positive size, no planes, mismatched array sizes, an Unknown type or a
    /// negative channel.
    PlaneBuffers(int64_t width, int64_t height, const std::vector<DataType>& types,
                 const std::vector<int32_t>& sourceChannels);

    /// A view of the planes as the block `levelRect` of `level`. Its slideRect is
    /// the level-0 edge mapping of the rect (ruling P9) with `levelScale`. Throws
    /// InvalidArgumentError when the rect's size is not the buffers' size.
    BlockView view(int32_t level, const Rect64& levelRect, double levelScale);

    /// Total bytes held, summed over the planes.
    int64_t bytes() const noexcept;

    int64_t width() const noexcept;
    int64_t height() const noexcept;
    int32_t planeCount() const noexcept;
    DataType planeType(int32_t plane) const;
    int32_t sourceChannel(int32_t plane) const;
    /// First byte of row 0 of the plane.
    uint8_t* planeData(int32_t plane);
    /// Bytes per row of the plane.
    int64_t rowStrideBytes(int32_t plane) const;

private:
    int64_t width_;
    int64_t height_;
    std::vector<DataType> types_;
    std::vector<int32_t> channels_;
    std::vector<std::vector<uint8_t>> planes_;
};

} // namespace slideio::processor::detail
