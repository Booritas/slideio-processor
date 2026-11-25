// Requirements: UR-EXT-5, UR-IMG-12
#include <slideio/processor/algorithm/registry.hpp>

#include <slideio/processor/core/error.hpp>
#include <slideio/processor/pipeline/operation_catalog.hpp>

#include "detail/builtin_operations.hpp"

#include <mutex>

namespace slideio::processor {

namespace {

std::mutex& registryMutex() {
    static std::mutex mutex;
    return mutex;
}

// Registers one built-in unless the catalog already holds its name, whoever
// registered it. The mutex serialises this module's registrations; a name
// registered by another caller in between makes registerFactory throw, which is
// swallowed once the name is known to be registered.
void registerOne(const char* name, OperationFactoryPtr (*makeFactory)()) {
    std::lock_guard<std::mutex> lock(registryMutex());
    OperationCatalog& catalog = OperationCatalog::instance();
    if (catalog.isRegistered(name)) {
        return;
    }
    try {
        catalog.registerFactory(makeFactory());
    } catch (const InvalidArgumentError&) {
        if (!catalog.isRegistered(name)) {
            throw;
        }
    }
}

// Registers the built-ins when the engine library is loaded. An exception
// escaping a static initialiser would terminate the process, so it is dropped;
// registerBuiltinOperations() can be called again to see it.
struct LoadTimeRegistration {
    LoadTimeRegistration() noexcept {
        try {
            registerBuiltinOperations();
        } catch (...) {
        }
    }
};

const LoadTimeRegistration loadTimeRegistration;

} // namespace

void registerBuiltinOperations() {
    registerPixelOperations();
    registerSegmentationOperations();
    registerMeasurementOperations();
}

void registerPixelOperations() {
    registerOne("pixel.threshold_fixed", &detail::makeFixedThresholdFactory);
    registerOne("pixel.threshold_otsu", &detail::makeOtsuThresholdFactory);
    registerOne("pixel.threshold_adaptive", &detail::makeAdaptiveThresholdFactory);
    registerOne("pixel.convert_type", &detail::makeTypeConvertFactory);
    registerOne("pixel.colour_convert", &detail::makeColourConvertFactory);
    registerOne("pixel.stain_deconvolution", &detail::makeStainDeconvolutionFactory);
    registerOne("pixel.gaussian_blur", &detail::makeGaussianBlurFactory);
    registerOne("pixel.median_blur", &detail::makeMedianBlurFactory);
    registerOne("pixel.sobel", &detail::makeSobelFactory);
    registerOne("pixel.scharr", &detail::makeScharrFactory);
    registerOne("pixel.laplacian", &detail::makeLaplacianFactory);
    registerOne("pixel.morph_erode", &detail::makeErodeFactory);
    registerOne("pixel.morph_dilate", &detail::makeDilateFactory);
    registerOne("pixel.morph_open", &detail::makeOpenFactory);
    registerOne("pixel.morph_close", &detail::makeCloseFactory);
    registerOne("pixel.distance_transform", &detail::makeDistanceTransformFactory);
    registerOne("pixel.bilateral_filter", &detail::makeBilateralFilterFactory);
    registerOne("pixel.canny", &detail::makeCannyFactory);
}

void registerSegmentationOperations() {
    registerOne("segment.connected_components", &detail::makeConnectedComponentsFactory);
    registerOne("segment.contours", &detail::makeContoursFactory);
    registerOne("segment.watershed", &detail::makeWatershedFactory);
    registerOne("segment.tissue_detect", &detail::makeTissueDetectFactory);
}

void registerMeasurementOperations() {}

} // namespace slideio::processor
