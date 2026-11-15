#include <slideio/processor/storage/raster_store.hpp>

#include <optional>
#include <string>

namespace slideio::processor {

// The constructor and the setters never validate (ruling S48); declare() does.
struct RasterDeclaration::Impl {
    std::optional<std::string> name;  // nullopt: the constructor was given null
    DataType type = DataType::Unknown;
    int32_t planeCount = 0;
    RasterCompression compression = RasterCompression::Deflate;
    std::string description;
};

RasterDeclaration::RasterDeclaration(const char* name, DataType type, int32_t planeCount)
    : impl_(std::make_unique<Impl>()) {
    if (name != nullptr) impl_->name = name;
    impl_->type = type;
    impl_->planeCount = planeCount;
}

RasterDeclaration::RasterDeclaration(const RasterDeclaration& other)
    : impl_(other.impl_ ? std::make_unique<Impl>(*other.impl_) : nullptr) {}

RasterDeclaration::RasterDeclaration(RasterDeclaration&& other) noexcept : impl_(std::move(other.impl_)) {}

RasterDeclaration& RasterDeclaration::operator=(const RasterDeclaration& other) {
    if (this != &other) {
        auto copy = other.impl_ ? std::make_unique<Impl>(*other.impl_) : nullptr;
        impl_ = std::move(copy);
    }
    return *this;
}

RasterDeclaration& RasterDeclaration::operator=(RasterDeclaration&& other) noexcept {
    if (this != &other) impl_ = std::move(other.impl_);
    return *this;
}

RasterDeclaration::~RasterDeclaration() = default;

RasterDeclaration& RasterDeclaration::compression(RasterCompression compression) {
    if (impl_) impl_->compression = compression;  // a moved-from declaration is left as it is
    return *this;
}

RasterDeclaration& RasterDeclaration::description(const char* text) {
    if (impl_) impl_->description = text != nullptr ? text : "";
    return *this;
}

const char* RasterDeclaration::name() const noexcept {
    return impl_ && impl_->name ? impl_->name->c_str() : nullptr;
}

DataType RasterDeclaration::planeType() const noexcept { return impl_ ? impl_->type : DataType::Unknown; }

int32_t RasterDeclaration::planeCount() const noexcept { return impl_ ? impl_->planeCount : 0; }

RasterCompression RasterDeclaration::compressionValue() const noexcept {
    return impl_ ? impl_->compression : RasterCompression::Deflate;
}

const char* RasterDeclaration::descriptionText() const noexcept { return impl_ ? impl_->description.c_str() : ""; }

} // namespace slideio::processor
