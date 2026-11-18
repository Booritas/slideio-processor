// Requirements: UR-PIX-3, UR-PIX-5, UR-QA-2, UR-IMG-11
#pragma once

#include <slideio/processor/core/block_view.hpp>
#include <slideio/processor/core/data_type.hpp>
#include <slideio/processor/core/geometry.hpp>

#include <cstdint>
#include <vector>

namespace slideio::processor::detail {

/// Evaluates a separable kernel over one plane in one fixed order, in double.
/// For each output pixel, each window row's weighted sum is formed left to
/// right with the horizontal kernel, then the column of those row sums is
/// weighted top to bottom with the vertical kernel. Row sums are shared
/// between the pixels of a tile, but each is a function of its own row's
/// samples alone, so every output value is exactly what the per-pixel order
/// gives and does not depend on the tiling. Immutable once built; apply() may
/// run concurrently.
class SeparableFilter {
public:
    /// Both kernels must have odd, non-zero lengths; else InternalError. The
    /// weights are used as given, with no normalisation.
    SeparableFilter(std::vector<double> horizontal, std::vector<double> vertical);

    /// Half the horizontal kernel's length.
    int32_t horizontalRadius() const noexcept;
    /// Half the vertical kernel's length.
    int32_t verticalRadius() const noexcept;

    /// The bytes apply() allocates per pixel of the tile grown by the kernel
    /// radii, for a tile of any size: its row of samples and its ring of row
    /// sums, 8 * (bv * w + w + bh - 1) bytes for a tile w wide, with bh and bv
    /// the horizontal and vertical kernel lengths. That is at most
    /// 8 * (bv + 1) * (w + bh - 1), so at most ceil(8 * (bv + 1) / bv) bytes per
    /// pixel of the (w + bh - 1) x (h + bv - 1) rect, since h >= 1.
    int64_t workingSetBytesPerPixel() const noexcept;

    /// The number of blocks apply() allocates, each of which may carry the
    /// allocator's overhead.
    static constexpr int64_t kWorkingSetBlocks = 2;

    /// Filters @p plane of @p input, which covers @p haloRectAtLevel, at every
    /// pixel of @p tileRectAtLevel, writing tile.height rows of tile.width
    /// doubles to @p out, rows @p outStride doubles apart. The input must hold
    /// the tile grown by the kernel radii, else InternalError. Samples are read
    /// through the sample codec as doubles, so the plane's type must be one
    /// whose samples a double holds exactly: not Int64 or UInt64 (InternalError).
    void apply(const BlockView& input, int32_t plane, const Rect64& haloRectAtLevel, const Rect64& tileRectAtLevel,
               double* out, int64_t outStride) const;

private:
    std::vector<double> horizontal_;
    std::vector<double> vertical_;
};

/// Writes @p values (output.height() rows of output.width() doubles, rows
/// @p stride doubles apart) to @p plane of @p output, which must be Float32
/// (each value rounded to nearest) or Float64 (exact); any other type throws
/// InternalError.
void writeFloatResult(BlockView& output, int32_t plane, const double* values, int64_t stride);

/// The types a filter computing in double through SeparableFilter accepts:
/// every sample type except Int64 and UInt64.
DataTypeSet floatFilterTypes();

/// The output type of such a filter: Float64 for Float64 input, else Float32.
DataType floatFilterOutputType(DataType input) noexcept;

} // namespace slideio::processor::detail
