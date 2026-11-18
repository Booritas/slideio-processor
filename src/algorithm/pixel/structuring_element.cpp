// Requirements: UR-PIX-4
#include <slideio/processor/algorithm/pixel/filter.hpp>

#include <slideio/processor/core/error.hpp>

#include "detail/cv_plane.hpp"
#include "detail/structuring_element_access.hpp"

#include <opencv2/imgproc.hpp>

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace slideio::processor {

namespace {

constexpr const char* kClass = "StructuringElement";
constexpr int32_t kMaximumSize = 255;

[[noreturn]] void throwBad(const char* parameter, const char* rule) {
    throw InvalidArgumentError((std::string(kClass) + ": parameter '" + parameter + "' " + rule).c_str());
}

void checkSize(const char* parameter, int32_t size) {
    if (size < 1 || size > kMaximumSize || size % 2 != 1) {
        throwBad(parameter, "must be an odd integer from 1 to 255");
    }
}

} // namespace

class StructuringElement::Impl {
public:
    int32_t width = 0;
    int32_t height = 0;
    bool custom = false;
    StructuringShape shape = StructuringShape::Rectangle;
    std::vector<uint8_t> mask; // width * height, row-major, 1 set and 0 clear
};

StructuringElement::StructuringElement(StructuringShape shape, int32_t width, int32_t height) {
    int cvShape = 0;
    switch (shape) {
    case StructuringShape::Rectangle:
        cvShape = cv::MORPH_RECT;
        break;
    case StructuringShape::Ellipse:
        cvShape = cv::MORPH_ELLIPSE;
        break;
    case StructuringShape::Cross:
        cvShape = cv::MORPH_CROSS;
        break;
    default:
        throwBad("shape", "is not a StructuringShape enumerator");
    }
    checkSize("width", width);
    checkSize("height", height);
    auto impl = std::make_unique<Impl>();
    impl->width = width;
    impl->height = height;
    impl->shape = shape;
    impl->mask.resize(static_cast<size_t>(width) * static_cast<size_t>(height));
    try {
        const cv::Mat drawn = cv::getStructuringElement(cvShape, cv::Size(width, height));
        for (int32_t y = 0; y < height; ++y) {
            const auto* row = drawn.ptr<uint8_t>(y);
            for (int32_t x = 0; x < width; ++x) {
                impl->mask[static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)] =
                    row[x] != 0 ? 1 : 0;
            }
        }
    } catch (const cv::Exception&) {
        detail::translateCv(kClass);
    }
    impl_ = std::move(impl);
}

StructuringElement::StructuringElement(const StructuringElement& other)
    : impl_(other.impl_ ? std::make_unique<Impl>(*other.impl_) : nullptr) {}

StructuringElement::StructuringElement(StructuringElement&& other) noexcept = default;

StructuringElement& StructuringElement::operator=(const StructuringElement& other) {
    if (this != &other) {
        std::unique_ptr<Impl> copy = other.impl_ ? std::make_unique<Impl>(*other.impl_) : nullptr;
        impl_ = std::move(copy);
    }
    return *this;
}

StructuringElement& StructuringElement::operator=(StructuringElement&& other) noexcept = default;

StructuringElement::~StructuringElement() = default;

StructuringElement StructuringElement::custom(const uint8_t* mask, int32_t width, int32_t height) {
    checkSize("width", width);
    checkSize("height", height);
    if (mask == nullptr) {
        throwBad("mask", "must not be null");
    }
    const size_t count = static_cast<size_t>(width) * static_cast<size_t>(height);
    std::vector<uint8_t> copied(count);
    bool anySet = false;
    for (size_t i = 0; i < count; ++i) {
        copied[i] = mask[i] != 0 ? 1 : 0;
        anySet = anySet || copied[i] != 0;
    }
    if (!anySet) {
        throwBad("mask", "must have at least one set pixel");
    }
    // Built as a 1 x 1 rectangle, then given the caller's mask.
    StructuringElement element(StructuringShape::Rectangle, 1, 1);
    element.impl_->width = width;
    element.impl_->height = height;
    element.impl_->custom = true;
    element.impl_->mask = std::move(copied);
    return element;
}

int32_t StructuringElement::width() const noexcept {
    return impl_ ? impl_->width : 0;
}

int32_t StructuringElement::height() const noexcept {
    return impl_ ? impl_->height : 0;
}

namespace detail {

bool StructuringElementAccess::valid(const StructuringElement& element) noexcept {
    return element.impl_ != nullptr;
}

bool StructuringElementAccess::isCustom(const StructuringElement& element) noexcept {
    return element.impl_->custom;
}

StructuringShape StructuringElementAccess::shape(const StructuringElement& element) noexcept {
    return element.impl_->shape;
}

const std::vector<uint8_t>& StructuringElementAccess::mask(const StructuringElement& element) noexcept {
    return element.impl_->mask;
}

} // namespace detail

} // namespace slideio::processor
