// Requirements: UR-IMG-1, UR-IMG-2, UR-IMG-9
#include <pipeline/test_support/synthetic_scene.hpp>

#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <string>
#include <utility>

namespace slideio::processor::testing_support {

namespace {

size_t sampleBytes(slideio::DataType type) {
    switch (type) {
    case slideio::DataType::DT_Byte:
    case slideio::DataType::DT_Int8:
        return 1;
    case slideio::DataType::DT_UInt16:
    case slideio::DataType::DT_Int16:
        return 2;
    case slideio::DataType::DT_UInt32:
    case slideio::DataType::DT_Int32:
    case slideio::DataType::DT_Float32:
        return 4;
    case slideio::DataType::DT_UInt64:
    case slideio::DataType::DT_Int64:
    case slideio::DataType::DT_Float64:
        return 8;
    default:
        // Float16 has no portable C++17 type; no pipeline test needs it from a scene yet.
        return 0;
    }
}

// Saturating: a double outside T's range makes static_cast undefined, so finite
// values are clamped first. NaN becomes 0 in an integer channel; NaN and
// infinities pass through to a floating-point one.
template <typename T>
void put(void* at, double v) {
    if constexpr (std::is_integral<T>::value) {
        if (std::isnan(v)) {
            v = 0.0;
        }
    } else {
        if (!std::isfinite(v)) {
            const T sample = static_cast<T>(v);
            std::memcpy(at, &sample, sizeof sample);
            return;
        }
    }
    T sample;
    if (!std::isnan(v) && v <= static_cast<double>(std::numeric_limits<T>::lowest())) {
        sample = std::numeric_limits<T>::lowest();
    } else if (!std::isnan(v) && v >= static_cast<double>(std::numeric_limits<T>::max())) {
        // For 64-bit integers max() rounds up to 2^63 or 2^64 as a double, so the
        // comparison catches every value that would overflow.
        sample = std::numeric_limits<T>::max();
    } else {
        sample = static_cast<T>(v);
    }
    std::memcpy(at, &sample, sizeof sample);
}

void writeSample(slideio::DataType type, void* at, double v) {
    switch (type) {
    case slideio::DataType::DT_Byte: put<uint8_t>(at, v); break;
    case slideio::DataType::DT_Int8: put<int8_t>(at, v); break;
    case slideio::DataType::DT_UInt16: put<uint16_t>(at, v); break;
    case slideio::DataType::DT_Int16: put<int16_t>(at, v); break;
    case slideio::DataType::DT_UInt32: put<uint32_t>(at, v); break;
    case slideio::DataType::DT_Int32: put<int32_t>(at, v); break;
    case slideio::DataType::DT_UInt64: put<uint64_t>(at, v); break;
    case slideio::DataType::DT_Int64: put<int64_t>(at, v); break;
    case slideio::DataType::DT_Float32: put<float>(at, v); break;
    case slideio::DataType::DT_Float64: put<double>(at, v); break;
    default: throw std::invalid_argument("SyntheticSource: unsupported channel type");
    }
}

} // namespace

SyntheticSource::SyntheticSource(std::vector<SyntheticLevel> levels,
                                 std::vector<slideio::DataType> channelTypes, PixelFunction pixel)
    : levels_(std::move(levels)), channelTypes_(std::move(channelTypes)), pixel_(std::move(pixel)) {
    if (levels_.empty() || channelTypes_.empty() || !pixel_) {
        throw std::invalid_argument("SyntheticSource: needs a level, a channel and a pixel function");
    }
    for (size_t i = 0; i < levels_.size(); ++i) {
        const SyntheticLevel& l = levels_[i];
        if (l.width <= 0 || l.height <= 0 || l.tileWidth < 0 || l.tileHeight < 0 ||
            (i > 0 && (l.width > levels_[i - 1].width || l.height > levels_[i - 1].height))) {
            throw std::invalid_argument("SyntheticSource: bad level " + std::to_string(i));
        }
    }
    for (slideio::DataType t : channelTypes_) {
        if (sampleBytes(t) == 0) {
            throw std::invalid_argument("SyntheticSource: unsupported channel type");
        }
    }
}

void SyntheticSource::readBlock(const slideio::Rect& sceneRect, const slideio::Size& blockSize,
                                const std::vector<int>& channelIndices, int, int, void* buffer,
                                size_t bufferSize) {
    fill(0, sceneRect, blockSize, channelIndices, buffer, bufferSize);
}

bool SyntheticSource::readLevelBlock(int level, const slideio::Rect& levelRect,
                                     const slideio::Size& blockSize,
                                     const std::vector<int>& channelIndices, int, int, void* buffer,
                                     size_t bufferSize) {
    fill(level, levelRect, blockSize, channelIndices, buffer, bufferSize);
    return true;
}

bool SyntheticSource::supportsConcurrentReads() const {
    return true;
}

void SyntheticSource::fill(int level, const slideio::Rect& rect, const slideio::Size& blockSize,
                           const std::vector<int>& channelIndices, void* buffer, size_t bufferSize) {
    if (level < 0 || static_cast<size_t>(level) >= levels_.size()) {
        throw std::invalid_argument("SyntheticSource: level out of range");
    }
    if (channelIndices.empty() || blockSize.width <= 0 || blockSize.height <= 0) {
        throw std::invalid_argument("SyntheticSource: empty request");
    }
    for (int c : channelIndices) {
        if (c < 0 || static_cast<size_t>(c) >= channelTypes_.size()) {
            throw std::invalid_argument("SyntheticSource: channel out of range");
        }
    }
    const slideio::DataType type = channelTypes_[static_cast<size_t>(channelIndices[0])];
    const size_t elem = sampleBytes(type);
    const size_t channels = channelIndices.size();
    const size_t bw = static_cast<size_t>(blockSize.width);
    const size_t bh = static_cast<size_t>(blockSize.height);
    if (bufferSize < bw * bh * channels * elem) {
        throw std::invalid_argument("SyntheticSource: buffer too small");
    }
    reads_.fetch_add(1, std::memory_order_relaxed);

    const SyntheticLevel& info = levels_[static_cast<size_t>(level)];
    auto* out = static_cast<unsigned char*>(buffer);
    for (size_t by = 0; by < bh; ++by) {
        // Nearest neighbour: the level pixel whose centre is nearest the block pixel's.
        const int64_t y = rect.y + static_cast<int64_t>((2 * by + 1) * static_cast<size_t>(rect.height) / (2 * bh));
        for (size_t bx = 0; bx < bw; ++bx) {
            const int64_t x =
                rect.x + static_cast<int64_t>((2 * bx + 1) * static_cast<size_t>(rect.width) / (2 * bw));
            const bool inside = x >= 0 && y >= 0 && x < info.width && y < info.height;
            for (size_t c = 0; c < channels; ++c) {
                const double v = inside ? pixel_(channelIndices[c], x, y) : 0.0;
                writeSample(type, out + ((by * bw + bx) * channels + c) * elem, v);
            }
        }
    }
}

slideio::SceneMetadata SyntheticSource::metadata() const {
    slideio::SceneMetadata m;
    m.name = "synthetic";
    const SyntheticLevel& base = levels_.front();
    m.rect = slideio::Rect(0, 0, static_cast<int32_t>(base.width), static_cast<int32_t>(base.height));
    m.channelDataTypes = channelTypes_;
    for (size_t i = 0; i < levels_.size(); ++i) {
        const SyntheticLevel& l = levels_[i];
        const double scale = static_cast<double>(l.width) / static_cast<double>(base.width);
        m.levels.emplace_back(static_cast<int>(i),
                              slideio::Size(static_cast<int32_t>(l.width), static_cast<int32_t>(l.height)),
                              scale, 0.0, slideio::Size(l.tileWidth, l.tileHeight));
    }
    return m;
}

int64_t SyntheticSource::readCount() const {
    return reads_.load(std::memory_order_relaxed);
}

std::shared_ptr<slideio::Scene> makeSyntheticScene(const std::shared_ptr<SyntheticSource>& source) {
    return slideio::makeScene(source, source->metadata());
}

std::shared_ptr<slideio::Scene> makeSyntheticScene(std::vector<SyntheticLevel> levels,
                                                   std::vector<slideio::DataType> channelTypes,
                                                   PixelFunction pixel) {
    return makeSyntheticScene(
        std::make_shared<SyntheticSource>(std::move(levels), std::move(channelTypes), std::move(pixel)));
}

} // namespace slideio::processor::testing_support
