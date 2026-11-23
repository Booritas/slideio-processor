// Requirements: UR-IMG-9, UR-IMG-4, UR-EXT-3
#include <pipeline/detail/scene_reader.hpp>

#include <pipeline/detail/level_table.hpp>

#include "core/detail/diagnostics.hpp"
#include <slideio/processor/core/error.hpp>

#include <slideio/core/levelinfo.hpp>
#include <slideio/slideio/scene.hpp>

#include <algorithm>
#include <cstring>
#include <exception>
#include <limits>
#include <new>
#include <string>
#include <tuple>
#include <vector>

namespace slideio::processor::detail {

namespace {

[[noreturn]] void rethrowAsIoError(const std::string& what) {
    try {
        throw;
    } catch (const ProcessorError&) {
        throw;
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::exception& e) {
        ErrorContext context;
        const std::string causeText = detail::diagnosticText(e);
        context.cause = causeText.c_str();
        throw IoError(what.c_str(), context);
    }
}

std::string channelText(size_t channel) {
    return "channel " + std::to_string(channel);
}

[[noreturn]] void throwUnknownType(size_t channel) {
    throw UnsupportedDataTypeError(("SceneReader: " + channelText(channel) + " has a sample type the pipeline does not support").c_str(),
                                   DataType::Unknown, DataTypeSet());
}

} // namespace

SceneReader::SceneReader(std::shared_ptr<slideio::Scene> scene) : scene_(std::move(scene)) {
    if (!scene_) {
        throw InvalidArgumentError("SceneReader: the scene is null");
    }
    // The slide the scene reports, named in SlideIO failures; "" when the scene cannot say.
    slidePath_ = sceneFilePath(*scene_);
    try {
        const int channels = scene_->getNumChannels();
        for (int c = 0; c < channels; ++c) {
            types_.push_back(dataTypeFromSlideio(static_cast<int32_t>(scene_->getChannelDataType(c))));
        }
        const std::tuple<int, int, int, int> rect = scene_->getRect();
        levels_ = levelTable(scene_->getNumZoomLevels(), std::get<2>(rect), std::get<3>(rect), [this](int32_t level) {
            const slideio::LevelInfo* info = scene_->getLevelInfo(level);
            if (info == nullptr) {
                throw IoError(withSlidePath("SceneReader: SlideIO reported no such level", slidePath_).c_str());
            }
            const slideio::Size size = info->getSize();
            const slideio::Size tile = info->getTileSize();
            return LevelDims{size.width, size.height, tile.width, tile.height};
        });
    } catch (...) {
        rethrowAsIoError(withSlidePath("SceneReader: SlideIO failed to describe the scene", slidePath_));
    }
}

const std::vector<LevelDims>& SceneReader::levels() const noexcept { return levels_; }

const std::string& SceneReader::slidePath() const noexcept { return slidePath_; }

std::vector<DataType> SceneReader::channelTypes() const {
    return types_;
}

void SceneReader::read(int32_t level, const Rect64& levelRect, const std::vector<int32_t>& channels,
                       PlaneBuffers& out) const {
    if (level < 0 || static_cast<size_t>(level) >= levels_.size()) {
        throw InvalidArgumentError("SceneReader::read: the level is out of range");
    }
    if (channels.empty()) {
        throw InvalidArgumentError("SceneReader::read: no channels selected");
    }
    if (levelRect.width <= 0 || levelRect.height <= 0 || levelRect.width != out.width() ||
        levelRect.height != out.height()) {
        throw InvalidArgumentError("SceneReader::read: the rectangle is empty or not the buffers' size");
    }
    if (static_cast<size_t>(out.planeCount()) != channels.size()) {
        throw InvalidArgumentError("SceneReader::read: one plane is needed per channel");
    }
    for (size_t i = 0; i < channels.size(); ++i) {
        const int32_t c = channels[i];
        if (c < 0 || static_cast<size_t>(c) >= types_.size()) {
            throw InvalidArgumentError("SceneReader::read: a channel is out of range");
        }
        if (types_[static_cast<size_t>(c)] == DataType::Unknown) {
            throwUnknownType(static_cast<size_t>(c));
        }
        if (out.planeType(static_cast<int32_t>(i)) != types_[static_cast<size_t>(c)] ||
            out.sourceChannel(static_cast<int32_t>(i)) != c) {
            throw InvalidArgumentError("SceneReader::read: a plane does not match its channel");
        }
    }

    // Everything outside the level stays zero, whatever SlideIO's background is.
    for (int32_t p = 0; p < out.planeCount(); ++p) {
        std::memset(out.planeData(p), 0, static_cast<size_t>(out.rowStrideBytes(p) * out.height()));
    }

    const LevelDims& size = levels_[static_cast<size_t>(level)];
    const int64_t x0 = std::max<int64_t>(levelRect.x, 0);
    const int64_t y0 = std::max<int64_t>(levelRect.y, 0);
    const int64_t x1 = std::min<int64_t>(levelRect.x + levelRect.width, size.width);
    const int64_t y1 = std::min<int64_t>(levelRect.y + levelRect.height, size.height);
    if (x1 <= x0 || y1 <= y0) {
        return;
    }
    const int64_t w = x1 - x0;
    const int64_t h = y1 - y0;
    if (w > std::numeric_limits<int>::max() || h > std::numeric_limits<int>::max() || x0 > std::numeric_limits<int>::max() ||
        y0 > std::numeric_limits<int>::max()) {
        throw InvalidArgumentError("SceneReader::read: the rectangle exceeds SlideIO's integer range");
    }
    const int64_t dstX = x0 - levelRect.x;
    const int64_t dstY = y0 - levelRect.y;

    const bool uniform = std::all_of(channels.begin(), channels.end(), [&](int32_t c) {
        return types_[static_cast<size_t>(c)] == types_[static_cast<size_t>(channels.front())];
    });

    // One group is one SlideIO call: every channel at once when the types agree,
    // else a single channel.
    const size_t groupSize = uniform ? channels.size() : 1;
    try {
        std::vector<uint8_t> buffer;
        for (size_t first = 0; first < channels.size(); first += groupSize) {
            const DataType type = out.planeType(static_cast<int32_t>(first));
            const size_t sample = static_cast<size_t>(dataTypeSizeBytes(type));
            std::vector<int> request;
            for (size_t i = first; i < first + groupSize; ++i) {
                request.push_back(static_cast<int>(channels[i]));
            }
            const size_t bytes = static_cast<size_t>(w) * static_cast<size_t>(h) * groupSize * sample;
            buffer.assign(bytes, 0);
            scene_->readResampledLevelBlockChannels(
                static_cast<int>(level),
                std::make_tuple(static_cast<int>(x0), static_cast<int>(y0), static_cast<int>(w), static_cast<int>(h)),
                std::make_tuple(static_cast<int>(w), static_cast<int>(h)), request, buffer.data(), buffer.size());
            // De-interleave into the planes, row by row.
            for (size_t k = 0; k < groupSize; ++k) {
                const int32_t plane = static_cast<int32_t>(first + k);
                uint8_t* dst = out.planeData(plane) + dstY * out.rowStrideBytes(plane) + dstX * static_cast<int64_t>(sample);
                for (int64_t y = 0; y < h; ++y) {
                    uint8_t* d = dst + y * out.rowStrideBytes(plane);
                    const uint8_t* s = buffer.data() + (static_cast<size_t>(y) * static_cast<size_t>(w) * groupSize + k) * sample;
                    for (int64_t x = 0; x < w; ++x) {
                        std::memcpy(d + x * static_cast<int64_t>(sample), s + static_cast<size_t>(x) * groupSize * sample, sample);
                    }
                }
            }
        }
    } catch (...) {
        rethrowAsIoError(withSlidePath("SceneReader::read: SlideIO failed to read the block", slidePath_));
    }
}

} // namespace slideio::processor::detail
