// Requirements: UR-STG-4, UR-RES-11
#include "detail/bag.hpp"

#include <slideio/processor/core/error.hpp>

namespace slideio::processor::detail {

BagReader::BagReader(const char* operationName, const OperationParameters& parameters)
    : operation_(operationName != nullptr ? operationName : ""), parameters_(parameters) {
    const int32_t count = parameters.count();
    keys_.reserve(static_cast<size_t>(count));
    for (int32_t i = 0; i < count; ++i) {
        keys_.emplace_back(parameters.keyAt(i));
    }
    read_.assign(keys_.size(), false);
}

bool BagReader::markRead(const char* key) {
    for (size_t i = 0; i < keys_.size(); ++i) {
        if (keys_[i] == key) {
            read_[i] = true;
            return true;
        }
    }
    return false;
}

void BagReader::throwBadValue(const char* key, const char* problem) const {
    throw InvalidArgumentError((operation_ + ": parameter '" + key + "' " + problem).c_str());
}

double BagReader::getDouble(const char* key, double fallback) {
    if (!markRead(key)) {
        return fallback;
    }
    double value = 0.0;
    if (!parameters_.getDouble(key, value)) {
        throwBadValue(key, "must be a double");
    }
    return value;
}

int64_t BagReader::getInt(const char* key, int64_t fallback) {
    if (!markRead(key)) {
        return fallback;
    }
    int64_t value = 0;
    if (!parameters_.getInt(key, value)) {
        throwBadValue(key, "must be an integer");
    }
    return value;
}

bool BagReader::getBool(const char* key, bool fallback) {
    if (!markRead(key)) {
        return fallback;
    }
    bool value = false;
    if (!parameters_.getBool(key, value)) {
        throwBadValue(key, "must be a boolean");
    }
    return value;
}

const char* BagReader::getString(const char* key, const char* fallback) {
    if (!markRead(key)) {
        return fallback;
    }
    const char* value = parameters_.getString(key, nullptr);
    if (value == nullptr) {
        throwBadValue(key, "must be a string");
    }
    return value;
}

bool BagReader::getDoubleArray(const char* key, std::vector<double>& out) {
    if (!markRead(key)) {
        return false;
    }
    const int32_t count = parameters_.getDoubleArray(key, nullptr, 0);
    if (count == 0) {
        // An empty array and a key of another type both report 0 elements.
        int64_t asInt = 0;
        double asDouble = 0.0;
        bool asBool = false;
        if (parameters_.getInt(key, asInt) || parameters_.getDouble(key, asDouble) || parameters_.getBool(key, asBool) ||
            parameters_.getString(key, nullptr) != nullptr) {
            throwBadValue(key, "must be an array of doubles");
        }
        out.clear();
        return true;
    }
    std::vector<double> values(static_cast<size_t>(count));
    parameters_.getDoubleArray(key, values.data(), count);
    out.swap(values);
    return true;
}

void BagReader::finish() const {
    for (size_t i = 0; i < keys_.size(); ++i) {
        if (!read_[i]) {
            throw InvalidArgumentError((operation_ + ": unknown parameter '" + keys_[i] + "'").c_str());
        }
    }
}

} // namespace slideio::processor::detail
