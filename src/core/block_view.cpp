#include <slideio/processor/core/block_view.hpp>

#include <slideio/processor/core/error.hpp>

#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace slideio::processor {

class BlockView::Impl {
public:
    int64_t width = 0;
    int64_t height = 0;
    Rect64 slideRect{0, 0, 0, 0};
    int32_t level = 0;
    std::vector<BlockPlane> planes;
};

namespace {

[[noreturn]] void fail(const std::string& message) {
    throw InvalidArgumentError(message.c_str());
}

std::string planeField(int32_t index, const char* field) {
    return "planes[" + std::to_string(index) + "]." + field;
}

const BlockPlane& planeAt(const std::vector<BlockPlane>* planes, int32_t index) {
    if (planes == nullptr) {
        throw InvalidArgumentError("plane index used on a null BlockView");
    }
    if (index < 0 || static_cast<size_t>(index) >= planes->size()) {
        throw InvalidArgumentError("plane index out of range");
    }
    return (*planes)[static_cast<size_t>(index)];
}

} // namespace

BlockView::BlockView() noexcept = default;

BlockView::BlockView(int64_t width, int64_t height,
                     Rect64 slideRect, int32_t level,
                     const BlockPlane* planes, int32_t planeCount) {
    if (width <= 0) {
        fail("width must be positive");
    }
    if (height <= 0) {
        fail("height must be positive");
    }
    if (level < 0) {
        fail("level must not be negative");
    }
    if (slideRect.isEmpty()) {
        fail("slideRect must not be empty");
    }
    if (planes == nullptr) {
        fail("planes must not be null");
    }
    if (planeCount < 1) {
        fail("planeCount must be at least 1");
    }
    for (int32_t i = 0; i < planeCount; ++i) {
        const BlockPlane& p = planes[i];
        if (p.data == nullptr) {
            fail(planeField(i, "data") + " must not be null");
        }
        const int32_t elementSize = dataTypeSizeBytes(p.type);
        if (elementSize <= 0) {
            fail(planeField(i, "type") + " must not be Unknown");
        }
        // An overflowing width * elementSize is treated as an undersized stride.
        if (width > std::numeric_limits<int64_t>::max() / elementSize
            || p.rowStrideBytes < width * elementSize) {
            fail(planeField(i, "rowStrideBytes") + " is smaller than width * element size");
        }
        if (p.sourceChannel < 0) {
            fail(planeField(i, "sourceChannel") + " must not be negative");
        }
    }
    auto impl = std::make_unique<Impl>();
    impl->width = width;
    impl->height = height;
    impl->slideRect = slideRect;
    impl->level = level;
    impl->planes.assign(planes, planes + planeCount);
    impl_ = std::move(impl);
}

BlockView::BlockView(const BlockView& other)
    : impl_(other.impl_ ? std::make_unique<Impl>(*other.impl_) : nullptr) {}

BlockView::BlockView(BlockView&& other) noexcept : impl_(std::move(other.impl_)) {}

BlockView& BlockView::operator=(const BlockView& other) {
    if (this != &other) {
        std::unique_ptr<Impl> copy = other.impl_ ? std::make_unique<Impl>(*other.impl_) : nullptr;
        impl_ = std::move(copy);
    }
    return *this;
}

BlockView& BlockView::operator=(BlockView&& other) noexcept {
    if (this != &other) {
        impl_ = std::move(other.impl_); // the source becomes null
    }
    return *this;
}

BlockView::~BlockView() = default;

bool BlockView::isNull() const noexcept { return !impl_; }

int64_t BlockView::width() const noexcept { return impl_ ? impl_->width : 0; }

int64_t BlockView::height() const noexcept { return impl_ ? impl_->height : 0; }

Rect64 BlockView::slideRect() const noexcept {
    return impl_ ? impl_->slideRect : Rect64{0, 0, 0, 0};
}

int32_t BlockView::level() const noexcept { return impl_ ? impl_->level : 0; }

int32_t BlockView::planeCount() const noexcept {
    return impl_ ? static_cast<int32_t>(impl_->planes.size()) : 0;
}

void* BlockView::planeData(int32_t plane) const {
    return planeAt(impl_ ? &impl_->planes : nullptr, plane).data;
}

DataType BlockView::planeType(int32_t plane) const {
    return planeAt(impl_ ? &impl_->planes : nullptr, plane).type;
}

int64_t BlockView::planeRowStrideBytes(int32_t plane) const {
    return planeAt(impl_ ? &impl_->planes : nullptr, plane).rowStrideBytes;
}

int32_t BlockView::planeSourceChannel(int32_t plane) const {
    return planeAt(impl_ ? &impl_->planes : nullptr, plane).sourceChannel;
}

} // namespace slideio::processor
