// Requirements: UR-SEG-3, UR-SEG-5
#pragma once

#include <slideio/processor/pipeline/operation.hpp>

namespace slideio::processor::detail {

/// Combines two partials of one channel (ruling P6a): counts and sums added,
/// the smaller minimum and the larger maximum taken. A partial with count 0
/// is the identity. Throws InvalidArgumentError when the channels differ.
/// Float sums are order-dependent, so a caller combining many partials does it
/// in canonical fragment order (SDS 05 5.7).
IntensityPartial combinePartials(const IntensityPartial& a, const IntensityPartial& b);

} // namespace slideio::processor::detail
