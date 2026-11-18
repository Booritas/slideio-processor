// Requirements: UR-EXE-2, UR-ALG-8
#include <slideio/processor/algorithm/pixel/colour.hpp>
#include <slideio/processor/algorithm/pixel/convert.hpp>
#include <slideio/processor/algorithm/pixel/threshold.hpp>
#include <slideio/processor/config/configuration.hpp>
#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/memory_budget.hpp>
#include <slideio/processor/pipeline/operation_catalog.hpp>
#include <slideio/processor/pipeline/operation_parameters.hpp>
#include <slideio/processor/pipeline/pipeline.hpp>

#include <algorithm/support/allocation_meter.hpp>
#include <algorithm/support/working_set_check.hpp>
#include <pipeline/detail/pipeline_impl.hpp>
#include <pipeline/detail/run_plan.hpp>
#include <pipeline/test_support/synthetic_scene.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace slideio::processor;
namespace ts = slideio::processor::testing_support;

namespace {

using ts::createOperation;
using ts::declaredWorkingSet;
using ts::expectApplyBounded;
using ts::expectGlobalBounded;
using ts::kAllDataTypes;

OperationPtr create(const char* name, const OperationParameters& parameters) {
    return createOperation(name, parameters);
}

OperationParameters otsuRange(int64_t binCount) {
    OperationParameters p;
    p.setDouble("histogramMinimum", -1000.0).setDouble("histogramMaximum", 1000.0).setInt("binCount", binCount);
    return p;
}

} // namespace

TEST(WorkingSetPixel, DeclarationsBoundTheMeasuredPeak) {
    // pixel.threshold_fixed: default and the far ends of its value, either direction.
    for (DataType type : kAllDataTypes) {
        expectApplyBounded("threshold_fixed default",
                           [] { return create("pixel.threshold_fixed", OperationParameters()); }, {type});
        expectApplyBounded("threshold_fixed extreme",
                           [] { return makeFixedThreshold(-1e300, ThresholdDirection::Below); }, {type});
    }

    // pixel.convert_type: every source to every target, saturating (overflow
    // counted, no error) under every rounding.
    for (DataType source : kAllDataTypes) {
        expectApplyBounded("convert_type default",
                           [] { return create("pixel.convert_type", OperationParameters()); }, {source});
        for (DataType target : kAllDataTypes) {
            expectApplyBounded(std::string("convert_type to ") + dataTypeName(target),
                               [target] { return makeTypeConvert(target, RoundingMode::Ceil, OverflowMode::Saturate); },
                               {source});
        }
    }

    // pixel.colour_convert: every conversion its inputs allow.
    const ColourSpace spaces[] = {ColourSpace::Rgb, ColourSpace::Grey, ColourSpace::Hsv, ColourSpace::Lab};
    for (ColourSpace from : spaces) {
        for (ColourSpace to : spaces) {
            if (from == to) {
                continue;
            }
            const auto make = [from, to] { return makeColourConvert(from, to); };
            const std::string label = "colour_convert " + std::to_string(static_cast<int32_t>(from)) + " to " +
                                      std::to_string(static_cast<int32_t>(to));
            if (from == ColourSpace::Rgb) {
                expectApplyBounded(label, make, {DataType::Byte, DataType::Byte, DataType::Byte});
            } else if (from == ColourSpace::Grey) {
                expectApplyBounded(label, make, {DataType::Byte});
                expectApplyBounded(label, make, {DataType::Float32});
            } else {
                expectApplyBounded(label, make, {DataType::Float32, DataType::Float32, DataType::Float32});
            }
        }
    }

    // pixel.stain_deconvolution: the default pair and three stains.
    const std::vector<DataType> rgb{DataType::Byte, DataType::Byte, DataType::Byte};
    expectApplyBounded("stain_deconvolution default",
                       [] { return create("pixel.stain_deconvolution", OperationParameters()); }, rgb);
    expectApplyBounded("stain_deconvolution three stains",
                       [] {
                           OperationParameters p;
                           const double v[] = {0.65, 0.70, 0.29, 0.07, 0.99, 0.11, 0.27, 0.57, 0.78};
                           p.setDoubleArray("vectors", v, 9);
                           return create("pixel.stain_deconvolution", p);
                       },
                       rgb);

    // pixel.threshold_otsu: one bin per value on the types binned so, and the
    // largest bin count with a range on every type.
    for (DataType type : {DataType::Byte, DataType::Int8, DataType::UInt16, DataType::Int16}) {
        expectGlobalBounded("threshold_otsu default",
                            [] { return create("pixel.threshold_otsu", OperationParameters()); }, {type});
    }
    for (DataType type : kAllDataTypes) {
        expectGlobalBounded("threshold_otsu 1048576 bins",
                            [] { return create("pixel.threshold_otsu", otsuRange(1048576)); }, {type});
    }

    // pixel.threshold_adaptive: the default window and the largest, Gaussian.
    for (DataType type : kAllDataTypes) {
        if (type == DataType::Int64 || type == DataType::UInt64) {
            continue; // Int64 and UInt64 are not supported.
        }
        expectApplyBounded("threshold_adaptive default",
                           [] { return create("pixel.threshold_adaptive", OperationParameters()); }, {type});
        expectApplyBounded("threshold_adaptive 1025 Gaussian",
                           [] { return makeAdaptiveThreshold(AdaptiveMethod::Gaussian, 1025, 2.0,
                                                             ThresholdDirection::Above); },
                           {type});
    }

    // The apply() helper of the shared harness agrees on a type it can encode.
    const OperationPtr adaptive = create("pixel.threshold_adaptive", OperationParameters());
    const int64_t harnessPeak = ts::measureApplyPeak(*adaptive, {DataType::Float32}, {0}, 64, 64,
                                                     [](int32_t, int64_t x, int64_t y) { return double(x ^ y); });
    EXPECT_LE(harnessPeak, declaredWorkingSet(*adaptive, 64, 64));
}

// Many channels in one tile, each with a tiny histogram: the per-call figure
// stays a bound however many channels the tile has, since checking that they
// are distinct allocates nothing.
TEST(WorkingSetPixel, OtsuBoundedOverManyChannels) {
    OperationParameters p;
    p.setDouble("histogramMinimum", 0.0).setDouble("histogramMaximum", 1.0).setInt("binCount", 2);
    const std::vector<DataType> planes(64, DataType::Float64);
    expectGlobalBounded("threshold_otsu 64 channels, 2 bins", [p] { return create("pixel.threshold_otsu", p); },
                        planes);
}

// A ceiling that holds a tile's buffers and the packed samples but not the
// histograms refuses the run before it starts, naming the operation.
TEST(WorkingSetPixel, OtsuRefusedWhenItsBinsDoNotFit) {
    auto source = std::make_shared<ts::SyntheticSource>(
        std::vector<ts::SyntheticLevel>{{1000, 800, 0, 0}},
        std::vector<slideio::DataType>{slideio::DataType::DT_Byte}, [](int, int64_t, int64_t) { return 0.0; });
    const auto scene = ts::makeSyntheticScene(source);
    const Configuration config =
        ConfigurationBuilder().tileSize(64, 64).tileSizeFromSlide(false).workerCount(2).build();
    PipelineSpec spec;
    spec.operation("pixel.threshold_otsu", otsuRange(1048576));

    Pipeline measured(scene, config, spec);
    measured.validate();
    const int64_t tileBytes = detail::PipelineAccess::plan(measured).tileBytes;
    const OperationPtr otsu = create("pixel.threshold_otsu", otsuRange(1048576));
    const int64_t perTile = otsu->workingSetBytesPerTile();
    ASSERT_GE(perTile, int64_t(16) * 1048576);
    const int64_t buffersAndSamples = tileBytes - perTile;

    Pipeline refused(scene, config, spec);
    refused.memoryBudget(makeMemoryBudget(buffersAndSamples + perTile / 2));
    std::string message;
    try {
        refused.validate();
    } catch (const InvalidArgumentError& e) {
        message = e.what();
    }
    EXPECT_NE(message.find("pixel.threshold_otsu"), std::string::npos) << message;
    EXPECT_NE(message.find("above the memory ceiling"), std::string::npos) << message;

    Pipeline fits(scene, config, spec);
    fits.memoryBudget(makeMemoryBudget(tileBytes));
    EXPECT_NO_THROW(fits.validate());
    EXPECT_EQ(source->readCount(), 0);
}
