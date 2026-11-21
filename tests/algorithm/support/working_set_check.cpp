// Requirements: UR-EXE-2, UR-ALG-8
#include <algorithm/support/working_set_check.hpp>

#include <slideio/processor/core/block_view.hpp>
#include <slideio/processor/core/flow.hpp>
#include <slideio/processor/core/object_record.hpp>
#include <slideio/processor/pipeline/operation_catalog.hpp>

#include <algorithm/support/allocation_meter.hpp>
#include <pipeline/detail/finishing_sink.hpp>
#include <pipeline/detail/operation_context_state.hpp>
#include <pipeline/detail/tile_evaluator.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <utility>

namespace slideio::processor::testing_support {

const int64_t kWorkingSetTileSizes[2] = {64, 257};

const DataType kAllDataTypes[11] = {DataType::Byte,    DataType::Int8,    DataType::UInt16, DataType::Int16,
                                    DataType::UInt32,  DataType::Int32,   DataType::Float16, DataType::Float32,
                                    DataType::Float64, DataType::Int64,   DataType::UInt64};

namespace {

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

// The run's object store, as far as an operation's apply() is concerned: the
// records are dropped, as storing them is the store's memory, not the
// operation's.
class DiscardSink : public ObjectSink {
public:
    void emit(const ObjectRecord&) override { ++emitted_; }
    void flush() override {}
    int64_t emittedCount() const override { return emitted_; }

private:
    int64_t emitted_ = 0;
};

std::string typeNames(const std::vector<DataType>& types) {
    std::string names;
    for (DataType type : types) {
        names += (names.empty() ? "" : ",") + std::string(dataTypeName(type));
    }
    return names;
}

} // namespace

int64_t measureWorkingSetPeak(Operation& op, const std::vector<DataType>& types, int64_t width, int64_t height,
                              WorkingSetCall call) {
    const Halo halo = op.halo();
    std::vector<std::vector<uint8_t>> in;
    for (DataType type : types) {
        in.push_back(patternBytes(type, width + halo.left + halo.right, height + halo.top + halo.bottom));
    }
    return measureWorkingSetPeakOn(op, types, in, width, height, call);
}

int64_t measureWorkingSetPeakOn(Operation& op, const std::vector<DataType>& types,
                                const std::vector<std::vector<uint8_t>>& in, int64_t width, int64_t height,
                                WorkingSetCall call, const WorkingSetAuxiliary* auxiliary) {
    const Halo halo = op.halo();
    const int64_t inWidth = width + halo.left + halo.right;
    const int64_t inHeight = height + halo.top + halo.bottom;
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
    // An operation producing objects emits them, and its seam fragments, as it
    // would in a run: each object through the run's own tile filter and
    // finishing sink, inside apply(); neither objects nor fragments are kept.
    DiscardSink store;
    detail::RunCounters counters;
    detail::FinishingSink finishing(store, {{LayerId(1), &op}}, 0.5, 0, &counters.droppedByFilter);
    detail::TileObjectFilter objects(&finishing, Rect64{0, 0, width, height}, nullptr, nullptr, counters);
    detail::SeamFragmentCallback fragments = [](const Polygon&, LayerId, const IntensityPartial*, int32_t, int64_t,
                                                int64_t) {};
    int64_t peak = 0;
    const auto once = [&](size_t firstIn, size_t inCount, size_t firstOut, size_t outCount) {
        std::vector<BlockPlane> inPlanes;
        for (size_t p = firstIn; p < firstIn + inCount; ++p) {
            inPlanes.push_back(BlockPlane{const_cast<uint8_t*>(in[p].data()), types[p], inWidth * dataTypeSizeBytes(types[p]),
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
        std::vector<BlockView> auxiliaryView;
        if (auxiliary != nullptr) {
            BlockPlane plane{const_cast<uint8_t*>(auxiliary->bytes.data()), auxiliary->type,
                             inWidth * dataTypeSizeBytes(auxiliary->type), 0};
            auxiliaryView.emplace_back(inWidth, inHeight, Rect64{-halo.left, -halo.top, inWidth, inHeight}, 0,
                                       &plane, 1);
        }
        detail::OperationContextState state;
        state.input = &input;
        state.output = call == WorkingSetCall::Apply ? &output : nullptr;
        state.level = 0;
        state.tileRectAtLevel = state.tileRect = Rect64{0, 0, width, height};
        state.haloRectAtLevel = state.haloRect = Rect64{-halo.left, -halo.top, inWidth, inHeight};
        state.levelScale = 1.0;
        state.tileIndex = 0;
        if (op.producesObjects()) {
            state.objectSink = &objects;
            state.targetLayer = LayerId(1);
            state.seamFragment = fragments;
        }
        if (auxiliary != nullptr) {
            state.auxiliaries.emplace_back(auxiliary->name, &auxiliaryView.front());
        }
        const auto context = detail::OperationContextAccess::make(std::move(state));
        AllocationMeter meter;
        if (call == WorkingSetCall::Apply) {
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

int64_t declaredWorkingSet(const Operation& op, int64_t width, int64_t height) {
    const Halo halo = op.halo();
    const int64_t area = (width + halo.left + halo.right) * (height + halo.top + halo.bottom);
    return area * std::max<int64_t>(0, op.workingSetBytesPerLevelPixel()) +
           std::max<int64_t>(0, op.workingSetBytesPerTile());
}

OperationPtr createOperation(const char* name, const OperationParameters& parameters) {
    return OperationCatalog::instance().create(name, parameters);
}

void expectApplyBounded(const std::string& label, const std::function<OperationPtr()>& make,
                        const std::vector<DataType>& types, const UnmeteredShare& share,
                        const std::vector<int64_t>& tiles) {
    for (int64_t tile : tiles) {
        const OperationPtr op = make();
        const int64_t peak = measureWorkingSetPeak(*op, types, tile, tile, WorkingSetCall::Apply);
        int64_t unmetered = 0;
        for (DataType type : types) {
            unmetered = std::max(unmetered, share ? share(*op, type, tile, tile) : int64_t(0));
        }
        EXPECT_LE(peak, declaredWorkingSet(*op, tile, tile) - unmetered)
            << label << " apply on " << typeNames(types) << " at tile " << tile << ": measured " << peak
            << ", declared " << declaredWorkingSet(*op, tile, tile) << ", unmetered share " << unmetered;
    }
}

void expectGlobalBounded(const std::string& label, const std::function<OperationPtr()>& make,
                         const std::vector<DataType>& types) {
    for (int64_t tile : kWorkingSetTileSizes) {
        const OperationPtr op = make();
        const int64_t accumulatePeak = measureWorkingSetPeak(*op, types, tile, tile, WorkingSetCall::Accumulate);
        EXPECT_LE(accumulatePeak, declaredWorkingSet(*op, tile, tile))
            << label << " accumulate on " << typeNames(types) << " at tile " << tile << ": measured "
            << accumulatePeak;
        op->finalizeAccumulation();
        const int64_t applyPeak = measureWorkingSetPeak(*op, types, tile, tile, WorkingSetCall::Apply);
        EXPECT_LE(applyPeak, declaredWorkingSet(*op, tile, tile))
            << label << " apply on " << typeNames(types) << " at tile " << tile << ": measured " << applyPeak;
    }
}

} // namespace slideio::processor::testing_support
