#include <slideio/processor/pipeline/backend.hpp>

#include <slideio/processor/core/error.hpp>

#include <opencv2/core.hpp>

#include <array>
#include <string>

namespace slideio::processor {

namespace {

struct DepthMapping {
    DataType type;
    int32_t depth;
};

// The unsigned 32-bit and the 64-bit integer depths exist from OpenCV 5 on.
constexpr std::array kDepths = {
    DepthMapping{DataType::Byte, CV_8U},
    DepthMapping{DataType::Int8, CV_8S},
    DepthMapping{DataType::UInt16, CV_16U},
    DepthMapping{DataType::Int16, CV_16S},
    DepthMapping{DataType::Int32, CV_32S},
    DepthMapping{DataType::Float16, CV_16F},
    DepthMapping{DataType::Float32, CV_32F},
    DepthMapping{DataType::Float64, CV_64F},
#if CV_VERSION_MAJOR >= 5
    DepthMapping{DataType::UInt32, CV_32U},
    DepthMapping{DataType::UInt64, CV_64U},
    DepthMapping{DataType::Int64, CV_64S},
#endif
};

} // namespace

class Backend::Impl {
public:
    Impl() {
        for (const DepthMapping& m : kDepths) {
            supported.add(m.type);
        }
    }

    DataTypeSet supported;
};

Backend::Backend() noexcept : impl_(std::make_unique<Impl>()) {}

Backend::~Backend() = default;

const Backend& Backend::instance() noexcept {
    static const Backend backend;
    return backend;
}

const char* Backend::name() const noexcept {
    return "OpenCV";
}

const char* Backend::version() const noexcept {
    static const std::string version = cv::getVersionString();
    return version.c_str();
}

bool Backend::supports(DataType type) const noexcept {
    return impl_->supported.contains(type);
}

DataTypeSet Backend::supportedTypes() const noexcept {
    return impl_->supported;
}

int32_t Backend::toBackendDepth(DataType type) const {
    for (const DepthMapping& m : kDepths) {
        if (m.type == type) {
            return m.depth;
        }
    }
    const std::string message = std::string("Data type ") + dataTypeName(type) +
                                " is not supported by the " + name() + " backend";
    throw UnsupportedDataTypeError(message.c_str(), type, impl_->supported);
}

DataType Backend::fromBackendDepth(int32_t depth) const noexcept {
    for (const DepthMapping& m : kDepths) {
        if (m.depth == depth) {
            return m.type;
        }
    }
    return DataType::Unknown;
}

} // namespace slideio::processor
