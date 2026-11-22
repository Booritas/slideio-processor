// Requirements: UR-STG-4, UR-RES-11
#include <slideio/processor/pipeline/operation_parameters.hpp>
#include "core/detail/diagnostics.hpp"

#include <slideio/processor/core/error.hpp>

#include <pipeline/detail/parameters_json.hpp>

#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <iterator>
#include <map>
#include <new>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace slideio::processor {

class OperationParameters::Impl {
public:
    using Value = std::variant<int64_t, double, bool, std::string, std::vector<double>>;
    // std::string orders bytewise (char_traits<char>). The transparent comparator
    // finds a const char* without building a std::string, so the noexcept getters
    // do not allocate.
    std::map<std::string, Value, std::less<>> entries;

    const Value* find(const char* key) const noexcept {
        if (key == nullptr) {
            return nullptr;
        }
        const auto it = entries.find(key);
        return it == entries.end() ? nullptr : &it->second;
    }
};

namespace {

void requireKey(const char* key, const char* setter) {
    if (key == nullptr || key[0] == '\0') {
        throw InvalidArgumentError((std::string("OperationParameters::") + setter +
                                    ": the key must be non-null and non-empty").c_str());
    }
}

void requireFinite(double value, const char* key, const char* setter) {
    if (!std::isfinite(value)) {
        throw InvalidArgumentError((std::string("OperationParameters::") + setter + ": the value of '" +
                                    key + "' is not finite; JSON cannot hold it").c_str());
    }
}

} // namespace

OperationParameters::OperationParameters() : impl_(std::make_unique<Impl>()) {}

OperationParameters::OperationParameters(const OperationParameters& other)
    : impl_(other.impl_ ? std::make_unique<Impl>(*other.impl_) : nullptr) {}

OperationParameters::OperationParameters(OperationParameters&& other) noexcept : impl_(std::move(other.impl_)) {
    other.impl_.reset();
}

OperationParameters& OperationParameters::operator=(const OperationParameters& other) {
    if (this != &other) {
        auto copy = other.impl_ ? std::make_unique<Impl>(*other.impl_) : nullptr;
        impl_ = std::move(copy);
    }
    return *this;
}

OperationParameters& OperationParameters::operator=(OperationParameters&& other) noexcept {
    if (this != &other) {
        impl_ = std::move(other.impl_);
        other.impl_.reset();
    }
    return *this;
}

OperationParameters::~OperationParameters() = default;

OperationParameters& OperationParameters::setInt(const char* key, int64_t value) {
    requireKey(key, "setInt");
    if (!impl_) {
        impl_ = std::make_unique<Impl>();
    }
    impl_->entries[key] = value;
    return *this;
}

OperationParameters& OperationParameters::setDouble(const char* key, double value) {
    requireKey(key, "setDouble");
    requireFinite(value, key, "setDouble");
    if (!impl_) {
        impl_ = std::make_unique<Impl>();
    }
    impl_->entries[key] = value;
    return *this;
}

OperationParameters& OperationParameters::setBool(const char* key, bool value) {
    requireKey(key, "setBool");
    if (!impl_) {
        impl_ = std::make_unique<Impl>();
    }
    impl_->entries[key] = value;
    return *this;
}

OperationParameters& OperationParameters::setString(const char* key, const char* value) {
    requireKey(key, "setString");
    if (!impl_) {
        impl_ = std::make_unique<Impl>();
    }
    impl_->entries[key] = std::string(value != nullptr ? value : "");
    return *this;
}

OperationParameters& OperationParameters::setDoubleArray(const char* key, const double* values, int32_t count) {
    requireKey(key, "setDoubleArray");
    if (count < 0 || (values == nullptr && count > 0)) {
        throw InvalidArgumentError((std::string("OperationParameters::setDoubleArray: '") + key +
                                    "' has a negative count or a null array with a positive count").c_str());
    }
    for (int32_t i = 0; i < count; ++i) {
        requireFinite(values[i], key, "setDoubleArray");
    }
    if (!impl_) {
        impl_ = std::make_unique<Impl>();
    }
    impl_->entries[key] = std::vector<double>(values, values + count);
    return *this;
}

bool OperationParameters::getInt(const char* key, int64_t& out) const noexcept {
    const Impl::Value* v = impl_ ? impl_->find(key) : nullptr;
    if (v == nullptr || !std::holds_alternative<int64_t>(*v)) {
        return false;
    }
    out = std::get<int64_t>(*v);
    return true;
}

bool OperationParameters::getDouble(const char* key, double& out) const noexcept {
    const Impl::Value* v = impl_ ? impl_->find(key) : nullptr;
    if (v == nullptr || !std::holds_alternative<double>(*v)) {
        return false;
    }
    out = std::get<double>(*v);
    return true;
}

bool OperationParameters::getBool(const char* key, bool& out) const noexcept {
    const Impl::Value* v = impl_ ? impl_->find(key) : nullptr;
    if (v == nullptr || !std::holds_alternative<bool>(*v)) {
        return false;
    }
    out = std::get<bool>(*v);
    return true;
}

const char* OperationParameters::getString(const char* key, const char* fallback) const noexcept {
    const Impl::Value* v = impl_ ? impl_->find(key) : nullptr;
    if (v == nullptr || !std::holds_alternative<std::string>(*v)) {
        return fallback;
    }
    return std::get<std::string>(*v).c_str();
}

int32_t OperationParameters::getDoubleArray(const char* key, double* buffer, int32_t capacity) const {
    const Impl::Value* v = impl_ ? impl_->find(key) : nullptr;
    if (v == nullptr || !std::holds_alternative<std::vector<double>>(*v)) {
        return 0;
    }
    const std::vector<double>& array = std::get<std::vector<double>>(*v);
    if (buffer != nullptr && capacity > 0) {
        const size_t n = std::min(array.size(), static_cast<size_t>(capacity));
        std::copy(array.begin(), array.begin() + static_cast<std::ptrdiff_t>(n), buffer);
    }
    return static_cast<int32_t>(array.size());
}

int32_t OperationParameters::count() const noexcept {
    return impl_ ? static_cast<int32_t>(impl_->entries.size()) : 0;
}

const char* OperationParameters::keyAt(int32_t index) const {
    if (index < 0 || index >= count()) {
        throw InvalidArgumentError("OperationParameters::keyAt: the index is out of range");
    }
    return std::next(impl_->entries.begin(), index)->first.c_str();
}

int32_t OperationParameters::toJson(char* buffer, int32_t capacity) const {
    std::string json;
    try {
        rapidjson::StringBuffer sb;
        rapidjson::Writer<rapidjson::StringBuffer> writer(sb);
        detail::parametersToJson(*this, writer);
        json.assign(sb.GetString(), sb.GetSize());
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::exception& e) {
        ErrorContext context;
        const std::string causeText = detail::diagnosticText(e);
        context.cause = causeText.c_str();
        throw InternalError("OperationParameters::toJson: serialisation failed", context);
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
