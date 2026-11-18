// Requirements: UR-IMG-7, UR-ALG-3
#include "detail/rgb_input.hpp"

#include <slideio/processor/core/data_type.hpp>
#include <slideio/processor/core/error.hpp>

#include <string>

namespace slideio::processor::detail {

void requireRgbInput(const BlockView& input, const char* operationName) {
    const int32_t count = input.planeCount();
    bool rgb = count == 3;
    for (int32_t plane = 0; plane < count; ++plane) {
        if (input.planeSourceChannel(plane) != plane || input.planeType(plane) != DataType::Byte) {
            rgb = false;
        }
    }
    if (rgb) {
        // An RGB input allocates nothing: the operations it guards declare no
        // working memory.
        return;
    }
    std::string found;
    for (int32_t plane = 0; plane < count; ++plane) {
        found += (plane == 0 ? "" : ", ") + std::to_string(input.planeSourceChannel(plane)) + " (" +
                 dataTypeName(input.planeType(plane)) + ")";
    }
    const std::string message = std::string(operationName) + ": needs channels 0, 1, 2 as RGB, Byte; found " +
                                std::to_string(count) + (count == 1 ? " channel" : " channels") +
                                (count == 0 ? "" : ": " + found);
    if (count != 3) {
        throw ChannelArityError(message.c_str(), count);
    }
    throw InvalidArgumentError(message.c_str());
}

} // namespace slideio::processor::detail
