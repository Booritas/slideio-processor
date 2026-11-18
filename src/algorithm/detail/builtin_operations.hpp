// Requirements: UR-EXT-5, UR-ALG-4
#pragma once

#include <slideio/processor/pipeline/operation_catalog.hpp>

namespace slideio::processor::detail {

// The factories of the built-in operations, one per operation, each defined in
// its operation's source file and registered by name in registry.cpp.

/// pixel.threshold_fixed
OperationFactoryPtr makeFixedThresholdFactory();

/// pixel.threshold_otsu
OperationFactoryPtr makeOtsuThresholdFactory();

/// pixel.threshold_adaptive
OperationFactoryPtr makeAdaptiveThresholdFactory();

/// pixel.colour_convert
OperationFactoryPtr makeColourConvertFactory();

/// pixel.convert_type
OperationFactoryPtr makeTypeConvertFactory();

/// pixel.stain_deconvolution
OperationFactoryPtr makeStainDeconvolutionFactory();

} // namespace slideio::processor::detail
