// Requirements: UR-EXE-2, UR-ALG-8
#include <slideio/processor/algorithm/pixel/colour.hpp>
#include <slideio/processor/algorithm/pixel/convert.hpp>
#include <slideio/processor/algorithm/pixel/threshold.hpp>
#include <slideio/processor/config/configuration.hpp>
#include <slideio/processor/core/block_view.hpp>
#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/memory_budget.hpp>
#include <slideio/processor/pipeline/operation_catalog.hpp>
#include <slideio/processor/pipeline/operation_parameters.hpp>
#include <slideio/processor/pipeline/pipeline.hpp>

#include <algorithm/support/allocation_meter.hpp>
#include <pipeline/detail/operation_context_state.hpp>
#include <pipeline/detail/pipeline_impl.hpp>
#include <pipeline/detail/run_plan.hpp>
#include <pipeline/test_support/synthetic_scene.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace slideio::processor;
namespace ts = slideio::processor::testing_support;

namespace {

const int64_t kTileSizes[] = {64, 257};

const DataType kAllTypes[] = {DataType::Byte,   DataType::Int8,  DataType::UInt16,  DataType::Int16,
                              DataType::UInt32, DataType::Int32, DataType::Float16, DataType::Float32,
                              DataType::Float64, DataType::Int64, DataType::UInt64};

// The sample bytes of one plane: a pattern that crosses every type's range,
// with finite binary16 values for Float16.
std::vector<uint8_t> patternBytes(DataType type, int64_t width, int64_t height) {
    const int64_t size = dataTypeSizeBytes(type);
    std::vector<uint8_t> bytes(static_cast<size_t>(width * height * size));
    for (int64_t i = 0; i < width * height; ++i) {
        const int64_t x = i % width;
        const int64_t y = i / width;
        const int64_t v = (x * 37 + y * 101) % 1000 - 300;
        uint8_t* at = &bytes[static_cast<size_t>(i * size)];
        switch (type) {
        case DataType::Byte: { const auto s = static_cast<uint8_t>(v & 0xFF); std::memcpy(at, &s, 1); break; }
        case DataType::Int8: { const auto s = static_cast<int8_t>(v % 128); std::memcpy(at, &s, 1); break; }
        case DataType::UInt16: { const auto s = static_cast<uint16_t>(v * 97); std::memcpy(at, &s, 2); break; }
        case DataType::Int16: { const auto s = static_cast<int16_t>(v * 31); std::memcpy(at, &s, 2); break; }
        case DataType::UInt32: { const auto s = static_cast<uint32_t>(v * 100003); std::memcpy(at, &s, 4); break; }
        case DataType::Int32: { const auto s = static_cast<int32_t>(v * 100003); std::memcpy(at, &s, 4); break; }
        case DataType::Int64: { const int64_t s = v * 1000000007LL; std::memcpy(at, &s, 8); break; }
        case DataType::UInt64: { const auto s = static_cast<uint64_t>(v * 1000000007LL); std::memcpy(at, &s, 8); break; }
        case DataType::Float16: {
            // Sign, a biased exponent from 1 to 30 and a mantissa: always finite.
            const auto s = static_cast<uint16_t>(((v & 1) << 15) | ((1 + (x + y) % 30) << 10) | ((x * 13) & 0x3FF));
            std::memcpy(at, &s, 2);
            break;
        }
        case DataType::Float32: { const auto s = static_cast<float>(v) * 1.5f; std::memcpy(at, &s, 4); break; }
        case DataType::Float64: { const double s = static_cast<double>(v) * 1.5; std::memcpy(at, &s, 8); break; }
        default: break;
        }
    }
    return bytes;
}

enum class Call { Apply, Accumulate };

// Calls @p op once per plane (PerChannel) or once on every plane (Joint) on a
// width x height tile grown by its halo, as the framework does, and returns the
// largest peak any one call allocates through operator new. Everything the
// call is given is built before the meter starts.
int64_t measure(Operation& op, const std::vector<DataType>& types, int64_t width, int64_t height, Call call) {
    const Halo halo = op.halo();
    const int64_t inWidth = width + halo.left + halo.right;
    const int64_t inHeight = height + halo.top + halo.bottom;
    std::vector<std::vector<uint8_t>> in;
    for (DataType type : types) {
        in.push_back(patternBytes(type, inWidth, inHeight));
    }
    const bool joint = op.arity() == ChannelArity::Joint;
    std::vector<DataType> outTypes;
    if (joint) {
        outTypes.assign(static_cast<size_t>(op.outputPlaneCount(static_cast<int32_t>(types.size()))),
                        op.outputType(types.front()));
    } else {
        for (DataType type : types) {
            outTypes.push_back(op.outputType(type));
        }
    }
    std::vector<std::vector<uint8_t>> out;
    for (DataType type : outTypes) {
        out.emplace_back(static_cast<size_t>(width * height * dataTypeSizeBytes(type)), uint8_t{0});
    }
    int64_t peak = 0;
    const auto once = [&](size_t firstIn, size_t inCount, size_t firstOut, size_t outCount) {
        std::vector<BlockPlane> inPlanes;
        for (size_t p = firstIn; p < firstIn + inCount; ++p) {
            inPlanes.push_back(BlockPlane{in[p].data(), types[p], inWidth * dataTypeSizeBytes(types[p]),
                                          static_cast<int32_t>(p)});
        }
        std::vector<BlockPlane> outPlanes;
        for (size_t p = firstOut; p < firstOut + outCount; ++p) {
            outPlanes.push_back(BlockPlane{out[p].data(), outTypes[p], width * dataTypeSizeBytes(outTypes[p]),
                                           static_cast<int32_t>(std::min(p, types.size() - 1))});
        }
        const BlockView input(inWidth, inHeight, Rect64{-halo.left, -halo.top, inWidth, inHeight}, 0,
                              inPlanes.data(), static_cast<int32_t>(inPlanes.size()));
        BlockView output(width, height, Rect64{0, 0, width, height}, 0, outPlanes.data(),
                         static_cast<int32_t>(outPlanes.size()));
        detail::OperationContextState state;
        state.input = &input;
        state.output = call == Call::Apply ? &output : nullptr;
        state.level = 0;
        state.tileRectAtLevel = state.tileRect = Rect64{0, 0, width, height};
        state.haloRectAtLevel = state.haloRect = Rect64{-halo.left, -halo.top, inWidth, inHeight};
        state.levelScale = 1.0;
        state.tileIndex = 0;
        const auto context = detail::OperationContextAccess::make(std::move(state));
        ts::AllocationMeter meter;
        if (call == Call::Apply) {
            op.apply(*context);
        } else {
            op.accumulate(*context);
        }
        peak = std::max(peak, meter.peakBytes());
    };
    if (joint) {
        once(0, types.size(), 0, outTypes.size());
    } else {
        for (size_t p = 0; p < types.size(); ++p) {
            once(p, 1, p, 1);
        }
    }
    return peak;
}

// What the operation declares for one call on a width x height tile.
int64_t declared(const Operation& op, int64_t width, int64_t height) {
    const Halo halo = op.halo();
    const int64_t area = (width + halo.left + halo.right) * (height + halo.top + halo.bottom);
    return area * std::max<int64_t>(0, op.workingSetBytesPerLevelPixel()) +
           std::max<int64_t>(0, op.workingSetBytesPerTile());
}

OperationPtr create(const char* name, const OperationParameters& parameters) {
    return OperationCatalog::instance().create(name, parameters);
}

std::string typeNames(const std::vector<DataType>& types) {
    std::string names;
    for (DataType type : types) {
        names += (names.empty() ? "" : ",") + std::string(dataTypeName(type));
    }
    return names;
}

// Checks that every apply() call's peak is within the declaration, on every
// tile size; a fresh operation per tile size, made by @p make.
void expectApplyBounded(const std::string& label, const std::function<OperationPtr()>& make,
                        const std::vector<DataType>& types) {
    for (int64_t tile : kTileSizes) {
        const OperationPtr op = make();
        const int64_t peak = measure(*op, types, tile, tile, Call::Apply);
        EXPECT_LE(peak, declared(*op, tile, tile))
            << label << " apply on " << typeNames(types) << " at tile " << tile << ": measured " << peak;
    }
}

// As expectApplyBounded, for accumulate() on a fresh operation (whose first
// call creates the channel's histogram), and for apply() once finalised.
void expectGlobalBounded(const std::string& label, const std::function<OperationPtr()>& make,
                         const std::vector<DataType>& types) {
    for (int64_t tile : kTileSizes) {
        const OperationPtr op = make();
        const int64_t accumulatePeak = measure(*op, types, tile, tile, Call::Accumulate);
        EXPECT_LE(accumulatePeak, declared(*op, tile, tile))
            << label << " accumulate on " << typeNames(types) << " at tile " << tile << ": measured "
            << accumulatePeak;
        op->finalizeAccumulation();
        const int64_t applyPeak = measure(*op, types, tile, tile, Call::Apply);
        EXPECT_LE(applyPeak, declared(*op, tile, tile))
            << label << " apply on " << typeNames(types) << " at tile " << tile << ": measured " << applyPeak;
    }
}

OperationParameters otsuRange(int64_t binCount) {
    OperationParameters p;
    p.setDouble("histogramMinimum", -1000.0).setDouble("histogramMaximum", 1000.0).setInt("binCount", binCount);
    return p;
}

} // namespace

TEST(WorkingSetPixel, DeclarationsBoundTheMeasuredPeak) {
    // pixel.threshold_fixed: default and the far ends of its value, either direction.
    for (DataType type : kAllTypes) {
        expectApplyBounded("threshold_fixed default",
                           [] { return create("pixel.threshold_fixed", OperationParameters()); }, {type});
        expectApplyBounded("threshold_fixed extreme",
                           [] { return makeFixedThreshold(-1e300, ThresholdDirection::Below); }, {type});
    }

    // pixel.convert_type: every source to every target, saturating (overflow
    // counted, no error) under every rounding.
    for (DataType source : kAllTypes) {
        expectApplyBounded("convert_type default",
                           [] { return create("pixel.convert_type", OperationParameters()); }, {source});
        for (DataType target : kAllTypes) {
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
    for (DataType type : kAllTypes) {
        expectGlobalBounded("threshold_otsu 1048576 bins",
                            [] { return create("pixel.threshold_otsu", otsuRange(1048576)); }, {type});
    }

    // pixel.threshold_adaptive: the default window and the largest, Gaussian.
    for (DataType type : kAllTypes) {
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
    EXPECT_LE(harnessPeak, declared(*adaptive, 64, 64));
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
    const int64_t bins = otsu->workingSetBytesPerTile();
    ASSERT_GE(bins, int64_t(16) * 1048576);
    const int64_t buffersAndSamples = tileBytes - bins;

    Pipeline refused(scene, config, spec);
    refused.memoryBudget(makeMemoryBudget(buffersAndSamples + bins / 2));
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
