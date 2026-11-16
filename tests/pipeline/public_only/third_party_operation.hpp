// Requirements: UR-EXT-1, UR-EXT-2
#pragma once

// An operation written as a third party would write it: this header and its
// source compile with only include/ and SlideIO's include directories on the
// path (tests/pipeline/CMakeLists.txt, pipeline_public_only), so nothing here
// can reach the engine's internals (UR-EXT-2).

#include <slideio/processor/pipeline/operation.hpp>
#include <slideio/processor/pipeline/operation_catalog.hpp>
#include <slideio/processor/pipeline/operation_parameters.hpp>

namespace thirdparty {

/// "thirdparty.invert": PerChannel, Byte, halo 1 (it reads no neighbour; the halo is
/// there to prove the tile offset is honoured), output 255 - sample - "bias"
/// (int parameter, default 0), floored at 0.
slideio::processor::OperationPtr makeInvertOperation(const slideio::processor::OperationParameters& parameters);

/// The factory of "thirdparty.invert", written against the public headers alone.
slideio::processor::OperationFactoryPtr makeInvertFactory();

} // namespace thirdparty
