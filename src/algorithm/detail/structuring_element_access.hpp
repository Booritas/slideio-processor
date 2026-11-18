// Requirements: UR-PIX-4
#pragma once

#include <slideio/processor/algorithm/pixel/filter.hpp>

#include <cstdint>
#include <vector>

namespace slideio::processor::detail {

/// What the morphology operations read of a StructuringElement beyond its
/// public width and height. Defined in pixel/structuring_element.cpp.
struct StructuringElementAccess {
    /// Whether @p element holds an element; false only for a moved-from one.
    static bool valid(const StructuringElement& element) noexcept;

    /// Whether @p element was built by StructuringElement::custom(). Requires valid().
    static bool isCustom(const StructuringElement& element) noexcept;

    /// The shape @p element was drawn from; meaningful only when !isCustom(). Requires valid().
    static StructuringShape shape(const StructuringElement& element) noexcept;

    /// The element's width * height pixels, row by row from the top, each 1
    /// when set and 0 otherwise. Requires valid().
    static const std::vector<uint8_t>& mask(const StructuringElement& element) noexcept;
};

} // namespace slideio::processor::detail
