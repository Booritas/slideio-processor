// Requirements: UR-ALG-6, UR-IMG-10, UR-IMG-11, UR-QA-2
#include <algorithm/support/harness.hpp>

#include <slideio/processor/config/configuration.hpp>
#include <slideio/processor/core/block_view.hpp>
#include <slideio/processor/core/control.hpp>
#include <slideio/processor/core/flow.hpp>
#include <slideio/processor/pipeline/pipeline.hpp>
#include <slideio/slideio/scene.hpp>

#include <algorithm/detail/sample_codec.hpp>
#include <pipeline/detail/operation_context_state.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <utility>

namespace slideio::processor::testing_support {

namespace {

template <typename T>
void put(uint8_t* at, double v) {
    T sample;
    if constexpr (std::is_integral<T>::value) {
        if (std::isnan(v)) {
            sample = 0;
        } else if (v <= static_cast<double>(std::numeric_limits<T>::lowest())) {
            sample = std::numeric_limits<T>::lowest();
        } else if (v >= static_cast<double>(std::numeric_limits<T>::max())) {
            sample = std::numeric_limits<T>::max();
        } else {
            sample = static_cast<T>(v);
        }
    } else {
        sample = static_cast<T>(v);
    }
    std::memcpy(at, &sample, sizeof sample);
}

void encode(DataType type, uint8_t* at, double v) {
    switch (type) {
    case DataType::Byte: put<uint8_t>(at, v); break;
    case DataType::Int8: put<int8_t>(at, v); break;
    case DataType::UInt16: put<uint16_t>(at, v); break;
    case DataType::Int16: put<int16_t>(at, v); break;
    case DataType::UInt32: put<uint32_t>(at, v); break;
    case DataType::Int32: put<int32_t>(at, v); break;
    case DataType::UInt64: put<uint64_t>(at, v); break;
    case DataType::Int64: put<int64_t>(at, v); break;
    case DataType::Float32: put<float>(at, v); break;
    case DataType::Float64: put<double>(at, v); break;
    default: throw std::invalid_argument("applyDirect: use applyDirectRaw for this type");
    }
}

BlockView viewOf(int64_t width, int64_t height, std::vector<std::vector<uint8_t>>& planes,
                 const std::vector<DataType>& types, const std::vector<int32_t>& channels, size_t first,
                 size_t count) {
    std::vector<BlockPlane> described;
    for (size_t p = first; p < first + count; ++p) {
        described.push_back(BlockPlane{planes[p].data(), types[p], width * dataTypeSizeBytes(types[p]),
                                       channels[p]});
    }
    return BlockView(width, height, Rect64{0, 0, width, height}, 0, described.data(),
                     static_cast<int32_t>(described.size()));
}

void applyOnce(Operation& op, const BlockView& input, BlockView& output, int64_t width, int64_t height) {
    detail::OperationContextState state;
    state.input = &input;
    state.output = &output;
    state.level = 0;
    state.tileRectAtLevel = state.haloRectAtLevel = state.tileRect = state.haloRect =
        Rect64{0, 0, width, height};
    state.levelScale = 1.0;
    state.tileIndex = 0;
    op.apply(*detail::OperationContextAccess::make(std::move(state)));
}

} // namespace

double Image::at(int32_t plane, int64_t x, int64_t y) const {
    const DataType type = types.at(static_cast<size_t>(plane));
    const uint8_t* row = planes.at(static_cast<size_t>(plane)).data() + y * width * dataTypeSizeBytes(type);
    const detail::Sample s = detail::readSample(row, x, type);
    switch (s.kind) {
    case detail::Sample::Kind::Signed: return static_cast<double>(s.s);
    case detail::Sample::Kind::Unsigned: return static_cast<double>(s.u);
    case detail::Sample::Kind::Float: return s.f;
    }
    return 0.0;
}

Image applyDirect(Operation& op, const std::vector<DataType>& inputTypes,
                  const std::vector<int32_t>& sourceChannels, int64_t width, int64_t height,
                  const std::function<double(int32_t plane, int64_t x, int64_t y)>& pixel) {
    std::vector<std::vector<uint8_t>> raw;
    for (size_t p = 0; p < inputTypes.size(); ++p) {
        const int32_t size = dataTypeSizeBytes(inputTypes[p]);
        std::vector<uint8_t> bytes(static_cast<size_t>(width * height * size), 0);
        for (int64_t y = 0; y < height; ++y) {
            for (int64_t x = 0; x < width; ++x) {
                encode(inputTypes[p], &bytes[static_cast<size_t>((y * width + x) * size)],
                       pixel(static_cast<int32_t>(p), x, y));
            }
        }
        raw.push_back(std::move(bytes));
    }
    return applyDirectRaw(op, inputTypes, sourceChannels, width, height, raw);
}

Image applyDirectRaw(Operation& op, const std::vector<DataType>& inputTypes,
                     const std::vector<int32_t>& sourceChannels, int64_t width, int64_t height,
                     const std::vector<std::vector<uint8_t>>& rawPlanes) {
    if (inputTypes.empty() || inputTypes.size() != sourceChannels.size() || inputTypes.size() != rawPlanes.size()) {
        throw std::invalid_argument("applyDirectRaw: one type, channel and plane per input plane");
    }
    std::vector<std::vector<uint8_t>> in = rawPlanes;
    for (size_t p = 0; p < in.size(); ++p) {
        if (in[p].size() != static_cast<size_t>(width * height * dataTypeSizeBytes(inputTypes[p]))) {
            throw std::invalid_argument("applyDirectRaw: a plane has the wrong byte count");
        }
    }

    Image out;
    out.width = width;
    out.height = height;
    std::vector<int32_t> outChannels;
    if (op.arity() == ChannelArity::Joint) {
        const int32_t count = op.outputPlaneCount(static_cast<int32_t>(inputTypes.size()));
        for (int32_t j = 0; j < count; ++j) {
            out.types.push_back(op.outputType(inputTypes.front()));
            outChannels.push_back(sourceChannels[std::min(static_cast<size_t>(j), sourceChannels.size() - 1)]);
        }
    } else {
        for (size_t p = 0; p < inputTypes.size(); ++p) {
            out.types.push_back(op.outputType(inputTypes[p]));
            outChannels.push_back(sourceChannels[p]);
        }
    }
    for (DataType type : out.types) {
        out.planes.emplace_back(static_cast<size_t>(width * height * dataTypeSizeBytes(type)), uint8_t{0});
    }

    if (op.arity() == ChannelArity::Joint) {
        const BlockView input = viewOf(width, height, in, inputTypes, sourceChannels, 0, in.size());
        BlockView output = viewOf(width, height, out.planes, out.types, outChannels, 0, out.planes.size());
        applyOnce(op, input, output, width, height);
    } else {
        for (size_t p = 0; p < in.size(); ++p) {
            const BlockView input = viewOf(width, height, in, inputTypes, sourceChannels, p, 1);
            BlockView output = viewOf(width, height, out.planes, out.types, outChannels, p, 1);
            applyOnce(op, input, output, width, height);
        }
    }
    return out;
}

namespace {

// Assembles the analysis level from the tiles, mapping each tile's level-0 rect
// to level pixels by the scene-to-level width ratio.
class AssemblingRasterSink : public RasterSink {
public:
    AssemblingRasterSink(int64_t width, int64_t height, double downsample)
        : width_(width), height_(height), downsample_(downsample) {}

    void writeTile(const BlockView& tile) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (types_.empty()) {
            for (int32_t p = 0; p < tile.planeCount(); ++p) {
                types_.push_back(tile.planeType(p));
                planes_.emplace_back(static_cast<size_t>(width_ * height_ * dataTypeSizeBytes(tile.planeType(p))),
                                     uint8_t{0});
            }
        }
        const Rect64 r = tile.slideRect();
        const int64_t x0 = std::llround(static_cast<double>(r.x) / downsample_);
        const int64_t y0 = std::llround(static_cast<double>(r.y) / downsample_);
        for (int32_t p = 0; p < tile.planeCount() && p < static_cast<int32_t>(types_.size()); ++p) {
            const int64_t size = dataTypeSizeBytes(types_[static_cast<size_t>(p)]);
            for (int64_t y = 0; y < tile.height() && y0 + y < height_; ++y) {
                const auto* row = static_cast<const uint8_t*>(tile.planeData(p)) + y * tile.planeRowStrideBytes(p);
                const int64_t columns = std::min(tile.width(), width_ - x0);
                std::memcpy(&planes_[static_cast<size_t>(p)][static_cast<size_t>(((y0 + y) * width_ + x0) * size)],
                            row, static_cast<size_t>(columns * size));
            }
        }
        ++tiles_;
    }
    void flush() override {}
    int64_t tilesWritten() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return tiles_;
    }

    Image image() const {
        std::lock_guard<std::mutex> lock(mutex_);
        Image out;
        out.width = width_;
        out.height = height_;
        out.types = types_;
        out.planes = planes_;
        return out;
    }

private:
    int64_t width_;
    int64_t height_;
    double downsample_;
    mutable std::mutex mutex_;
    std::vector<DataType> types_;
    std::vector<std::vector<uint8_t>> planes_;
    int64_t tiles_ = 0;
};

} // namespace

Image runEager(std::shared_ptr<slideio::Scene> scene, const PipelineSpec& spec, int32_t tileSize,
               int32_t workers) {
    const Configuration config =
        ConfigurationBuilder().tileSize(tileSize, tileSize).tileSizeFromSlide(false).workerCount(workers).build();
    const int sceneWidth = std::get<2>(scene->getRect());
    Pipeline pipeline(scene, config, spec);
    pipeline.validate();
    const Rect64 level = pipeline.rect(0);
    AssemblingRasterSink sink(level.width, level.height,
                              static_cast<double>(sceneWidth) / static_cast<double>(level.width));
    pipeline.rasterSink(&sink);
    pipeline.run(nullptr, CancellationToken());
    return sink.image();
}

} // namespace slideio::processor::testing_support
