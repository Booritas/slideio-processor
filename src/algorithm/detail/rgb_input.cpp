// Requirements: UR-IMG-7, UR-ALG-3
#include "detail/rgb_input.hpp"

#include <slideio/processor/core/data_type.hpp>
#include <slideio/processor/core/error.hpp>

#include <string>

namespace slideio::processor::detail {

void requireRgbInput(const BlockView& input, const char* operationName) {
    const int32_t count = input.planeCount();
    bool rgb = count == 3;
    std::string found;
    for (int32_t plane = 0; plane < count; ++plane) {
        const int32_t channel = input.planeSourceChannel(plane);
        const DataType type = input.planeType(plane);
        if (channel != plane || type != DataType::Byte) {
            rgb = false;
        }
        found += (plane == 0 ? "" : ", ") + std::to_string(channel) + " (" + dataTypeName(type) + ")";
    }
    if (rgb) {
        return;
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
