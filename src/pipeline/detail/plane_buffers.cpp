// Requirements: UR-IMG-9, UR-IMG-4
#include <pipeline/detail/plane_buffers.hpp>

#include <core/detail/level_mapping.hpp>

#include <slideio/processor/core/error.hpp>

#include <limits>
#include <string>

namespace slideio::processor::detail {

PlaneBuffers::PlaneBuffers(int64_t width, int64_t height, const std::vector<DataType>& types,
                           const std::vector<int32_t>& sourceChannels)
    : width_(width), height_(height), types_(types), channels_(sourceChannels) {
    if (width <= 0 || height <= 0) {
        throw InvalidArgumentError("PlaneBuffers: width and height must be positive");
    }
    if (types.empty()) {
        throw InvalidArgumentError("PlaneBuffers: at least one plane is required");
    }
    if (types.size() != sourceChannels.size()) {
        throw InvalidArgumentError("PlaneBuffers: types and sourceChannels differ in size");
    }
    planes_.reserve(types.size());
    for (size_t i = 0; i < types.size(); ++i) {
        const int32_t size = types[i] == DataType::Unknown ? 0 : dataTypeSizeBytes(types[i]);
        if (size <= 0) {
            throw InvalidArgumentError(("PlaneBuffers: plane " + std::to_string(i) + " has no sample type").c_str());
        }
        if (sourceChannels[i] < 0) {
            throw InvalidArgumentError(("PlaneBuffers: plane " + std::to_string(i) + " has a negative channel").c_str());
        }
        if (width > std::numeric_limits<int64_t>::max() / size / height) {
            throw InvalidArgumentError("PlaneBuffers: the block is too large");
        }
        planes_.emplace_back(static_cast<size_t>(width * size * height), static_cast<uint8_t>(0));
    }
}

BlockView PlaneBuffers::view(int32_t level, const Rect64& levelRect, double levelScale) {
    if (levelRect.width != width_ || levelRect.height != height_) {
        throw InvalidArgumentError("PlaneBuffers::view: the rectangle's size is not the buffers' size");
    }
    std::vector<BlockPlane> planes;
    planes.reserve(planes_.size());
    for (size_t i = 0; i < planes_.size(); ++i) {
        planes.push_back(BlockPlane{planes_[i].data(), types_[i], width_ * dataTypeSizeBytes(types_[i]), channels_[i]});
    }
    return BlockView(width_, height_, rectToLevel0(levelRect, levelScale), level, planes.data(),
                     static_cast<int32_t>(planes.size()));
}

int64_t PlaneBuffers::bytes() const noexcept {
    int64_t total = 0;
    for (const std::vector<uint8_t>& p : planes_) {
        total += static_cast<int64_t>(p.size());
    }
    return total;
}

int64_t PlaneBuffers::width() const noexcept {
    return width_;
}

int64_t PlaneBuffers::height() const noexcept {
    return height_;
}

int32_t PlaneBuffers::planeCount() const noexcept {
    return static_cast<int32_t>(planes_.size());
}

DataType PlaneBuffers::planeType(int32_t plane) const {
    return types_.at(static_cast<size_t>(plane));
}

int32_t PlaneBuffers::sourceChannel(int32_t plane) const {
    return channels_.at(static_cast<size_t>(plane));
}

uint8_t* PlaneBuffers::planeData(int32_t plane) {
    return planes_.at(static_cast<size_t>(plane)).data();
}

int64_t PlaneBuffers::rowStrideBytes(int32_t plane) const {
    return width_ * dataTypeSizeBytes(types_.at(static_cast<size_t>(plane)));
}

} // namespace slideio::processor::detail
