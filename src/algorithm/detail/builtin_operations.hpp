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

/// pixel.gaussian_blur
OperationFactoryPtr makeGaussianBlurFactory();

/// pixel.median_blur
OperationFactoryPtr makeMedianBlurFactory();

/// pixel.distance_transform
OperationFactoryPtr makeDistanceTransformFactory();

/// pixel.fill_holes
OperationFactoryPtr makeFillHolesFactory();

/// pixel.sobel
OperationFactoryPtr makeSobelFactory();

/// pixel.scharr
OperationFactoryPtr makeScharrFactory();

/// pixel.laplacian
OperationFactoryPtr makeLaplacianFactory();

/// pixel.morph_erode
OperationFactoryPtr makeErodeFactory();

/// pixel.morph_dilate
OperationFactoryPtr makeDilateFactory();

/// pixel.morph_open
OperationFactoryPtr makeOpenFactory();

/// pixel.morph_close
OperationFactoryPtr makeCloseFactory();

/// pixel.bilateral_filter
OperationFactoryPtr makeBilateralFilterFactory();

/// pixel.canny
OperationFactoryPtr makeCannyFactory();

/// segment.connected_components
OperationFactoryPtr makeConnectedComponentsFactory();

/// segment.contours
OperationFactoryPtr makeContoursFactory();

/// segment.watershed
OperationFactoryPtr makeWatershedFactory();

/// segment.tissue_detect
OperationFactoryPtr makeTissueDetectFactory();

} // namespace slideio::processor::detail
