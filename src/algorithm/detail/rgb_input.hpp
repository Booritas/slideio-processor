// Requirements: UR-IMG-7, UR-ALG-3
#pragma once

#include <slideio/processor/core/block_view.hpp>

namespace slideio::processor::detail {

/// Checks that @p input is an RGB block: exactly three planes, each Byte,
/// holding scene channels 0, 1 and 2 in that order. A scene states no colour
/// model, so RGB is inferred from the channels. Throws ChannelArityError for
/// another plane count and InvalidArgumentError for another type or channel
/// order; the message names @p operationName, says it needs channels 0, 1, 2
/// as RGB, Byte, and lists the channels found (indices and types only).
void requireRgbInput(const BlockView& input, const char* operationName);

} // namespace slideio::processor::detail
