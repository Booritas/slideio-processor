// Requirements: UR-SEG-3, UR-SEG-4, UR-SEG-5, UR-IMG-2
#pragma once

#include "detail/region_trace.hpp"

#include <slideio/processor/core/block_view.hpp>
#include <slideio/processor/pipeline/operation.hpp>

#include <cstdint>

namespace slideio::processor::detail {

/// Emits the traced regions of one tile. @p labels is the buffer @p traced was
/// traced from: one int32 label per pixel of context.tileRectAtLevel(), row y
/// at labels + y * stride. Each region's rings are moved to the tile rect and
/// mapped to level 0 through context.toLevel0(); outer rings come first in the
/// polygon.
///
/// Each region is measured over its own pixels in every plane of @p input
/// from @p firstIntensityPlane on (none when that is the plane count or more):
/// one IntensityPartial per plane, its channel the plane's source channel,
/// accumulated in double in row-major order over the tile; NaN samples are
/// skipped, infinite ones counted. @p input covers context.haloRectAtLevel(),
/// as context.input() does.
///
/// A region touching the tile's edge goes to context.emitSeamFragment() with
/// its partials, since the rest of it may lie in another tile, and its join
/// key: @p joinKeys[r] for region r of @p traced, or 0 for every region when
/// @p joinKeys is null. The merger joins it only with fragments of an equal
/// key, so a key must name the region identically in every tile. Any other is
/// a finished object on context.targetLayer() with applyStandardMeasurements()
/// over its polygon and partials, emitted to context.objectSink(); when the
/// run supplied no object sink it is not emitted. Throws what
/// OperationContext::toLevel0() and emitSeamFragment() throw.
///
/// Memory: one IntensityPartial (48 bytes) per region and measured plane, and
/// one buffer of the longest ring's vertices, both allocated once; then, one
/// region at a time, its polygon and its record.
void emitRegions(OperationContext& context, const int32_t* labels, int64_t stride, const TracedRegions& traced,
                 const BlockView& input, int32_t firstIntensityPlane, const int64_t* joinKeys = nullptr);

/// The scene's samples a segmenter measures for planes @p first onward of
/// context.input(): for each, the first plane of context.sourceInput() with
/// the same source channel, each channel once, so `intensity.<c>.*` measures
/// channel c as read, whatever the earlier operations of the chain did to it,
/// and planes carrying a channel already measured add nothing. Same geometry as
/// context.input(); a null view when there is no plane from @p first on.
/// Throws InternalError naming @p operation when sourceInput() differs from
/// input() in size or holds no plane of one of the channels.
BlockView measuredPlanes(const OperationContext& context, int32_t first, const char* operation);

} // namespace slideio::processor::detail
