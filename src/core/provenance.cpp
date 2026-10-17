#include <slideio/processor/core/provenance.hpp>

#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/version.hpp>

#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <chrono>
#include <cstring>
#include <exception>
#include <string>
#include <utility>

namespace slideio::processor {

class Provenance::Impl {
public:
    Impl()
        : createdAtUnixMs(std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::system_clock::now().time_since_epoch())
                              .count()) {}

    std::string slideId;
    int32_t sceneIndex = 0;
    int64_t createdAtUnixMs;
};

Provenance::Provenance() : impl_(std::make_unique<Impl>()) {}

Provenance::Provenance(const Provenance& other)
    : impl_(other.impl_ ? std::make_unique<Impl>(*other.impl_) : nullptr) {}

Provenance::Provenance(Provenance&& other) noexcept : impl_(std::move(other.impl_)) {
    other.impl_.reset();
}

Provenance& Provenance::operator=(const Provenance& other) {
    if (this != &other) {
        auto copy = other.impl_ ? std::make_unique<Impl>(*other.impl_) : nullptr;
        impl_ = std::move(copy);
    }
    return *this;
}

Provenance& Provenance::operator=(Provenance&& other) noexcept {
    if (this != &other) {
        impl_ = std::move(other.impl_);
        other.impl_.reset();
    }
    return *this;
}

Provenance::~Provenance() = default;

const char* Provenance::slideId() const noexcept { return impl_ ? impl_->slideId.c_str() : ""; }

int32_t Provenance::sceneIndex() const noexcept { return impl_ ? impl_->sceneIndex : 0; }

const char* Provenance::libraryVersion() const noexcept { return slideio::processor::libraryVersion(); }

int64_t Provenance::createdAtUnixMs() const noexcept { return impl_ ? impl_->createdAtUnixMs : 0; }

void Provenance::setSlideId(const char* slideId) {
    if (slideId == nullptr) {
        throw InvalidArgumentError("Provenance::setSlideId: slideId must not be null");
    }
    std::string copy(slideId);
    if (!impl_) {
        impl_ = std::make_unique<Impl>();
    }
    impl_->slideId = std::move(copy);
}

void Provenance::setSceneIndex(int32_t sceneIndex) {
    if (sceneIndex < 0) {
        throw InvalidArgumentError("Provenance::setSceneIndex: sceneIndex must not be negative");
    }
    if (!impl_) {
        impl_ = std::make_unique<Impl>();
    }
    impl_->sceneIndex = sceneIndex;
}

int32_t Provenance::toJson(char* buffer, int32_t capacity) const {
    std::string json;
    try {
        rapidjson::StringBuffer sb;
        rapidjson::Writer<rapidjson::StringBuffer> w(sb);
        const char* id = slideId();
        w.StartObject();
        w.Key("slideId");
        w.String(id, static_cast<rapidjson::SizeType>(std::strlen(id)));
        w.Key("sceneIndex");
        w.Int(sceneIndex());
        w.Key("libraryVersion");
        w.String(libraryVersion());
        w.Key("createdAtUnixMs");
        w.Int64(createdAtUnixMs());
        w.EndObject();
        json.assign(sb.GetString(), sb.GetSize());
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::exception& e) {
        ErrorContext context;
        context.cause = e.what();
        throw InternalError("Provenance::toJson: serialisation failed", context);
    }
    const int32_t length = static_cast<int32_t>(json.size());
    if (buffer != nullptr && capacity > 0) {
        const int32_t n = length < capacity - 1 ? length : capacity - 1;
        std::memcpy(buffer, json.data(), static_cast<size_t>(n));
        buffer[n] = '\0';
    }
    return length;
}

} // namespace slideio::processor
