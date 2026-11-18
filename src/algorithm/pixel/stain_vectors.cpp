// Requirements: UR-PIX-2
#include <slideio/processor/algorithm/pixel/colour.hpp>

#include <slideio/processor/core/error.hpp>

#include <cmath>
#include <string>

namespace slideio::processor {

namespace {

constexpr int32_t kMaxStains = 3;

[[noreturn]] void throwInvalid(const char* method, const char* problem) {
    throw InvalidArgumentError((std::string("StainVectors::") + method + ": " + problem).c_str());
}

bool validComponent(double value) {
    return std::isfinite(value) && value >= 0.0;
}

} // namespace

class StainVectors::Impl {
public:
    int32_t count = 0;
    double vectors[kMaxStains][3] = {};
    std::string names[kMaxStains];
};

StainVectors::StainVectors() : impl_(std::make_unique<Impl>()) {}

StainVectors::StainVectors(const StainVectors& other)
    : impl_(other.impl_ ? std::make_unique<Impl>(*other.impl_) : nullptr) {}

StainVectors::StainVectors(StainVectors&& other) noexcept : impl_(std::move(other.impl_)) {
    other.impl_.reset();
}

StainVectors& StainVectors::operator=(const StainVectors& other) {
    if (this != &other) {
        auto copy = other.impl_ ? std::make_unique<Impl>(*other.impl_) : nullptr;
        impl_ = std::move(copy);
    }
    return *this;
}

StainVectors& StainVectors::operator=(StainVectors&& other) noexcept {
    if (this != &other) {
        impl_ = std::move(other.impl_);
        other.impl_.reset();
    }
    return *this;
}

StainVectors::~StainVectors() = default;

StainVectors StainVectors::hematoxylinEosin() noexcept {
    // An exception here reaches noexcept and terminates, as documented.
    StainVectors stains;
    stains.setVector(0, 0.650, 0.704, 0.286);
    stains.setVector(1, 0.072, 0.990, 0.105);
    stains.setStainName(0, "Hematoxylin");
    stains.setStainName(1, "Eosin");
    return stains;
}

StainVectors StainVectors::hematoxylinDab() noexcept {
    StainVectors stains;
    stains.setVector(0, 0.650, 0.704, 0.286);
    stains.setVector(1, 0.268, 0.570, 0.776);
    stains.setStainName(0, "Hematoxylin");
    stains.setStainName(1, "DAB");
    return stains;
}

int32_t StainVectors::count() const noexcept {
    return impl_ ? impl_->count : 0;
}

void StainVectors::setVector(int32_t index, double r, double g, double b) {
    const int32_t held = count();
    if (index < 0 || index > held) {
        throwInvalid("setVector", "the index is outside 0 to count()");
    }
    if (index >= kMaxStains) {
        throwInvalid("setVector", "at most three stains are held");
    }
    if (!validComponent(r) || !validComponent(g) || !validComponent(b)) {
        throwInvalid("setVector", "a component is negative, NaN or infinite");
    }
    if (r == 0.0 && g == 0.0 && b == 0.0) {
        throwInvalid("setVector", "all three components are zero");
    }
    if (!impl_) {
        impl_ = std::make_unique<Impl>();
    }
    impl_->vectors[index][0] = r;
    impl_->vectors[index][1] = g;
    impl_->vectors[index][2] = b;
    if (index == impl_->count) {
        impl_->names[index].clear();
        ++impl_->count;
    }
}

void StainVectors::getVector(int32_t index, double& r, double& g, double& b) const {
    if (index < 0 || index >= count()) {
        throwInvalid("getVector", "the index is outside 0 to count() - 1");
    }
    r = impl_->vectors[index][0];
    g = impl_->vectors[index][1];
    b = impl_->vectors[index][2];
}

const char* StainVectors::stainName(int32_t index) const {
    if (index < 0 || index >= count()) {
        throwInvalid("stainName", "the index is outside 0 to count() - 1");
    }
    return impl_->names[index].c_str();
}

void StainVectors::setStainName(int32_t index, const char* name) {
    if (index < 0 || index >= count()) {
        throwInvalid("setStainName", "the index is outside 0 to count() - 1");
    }
    if (name == nullptr) {
        throwInvalid("setStainName", "the name is null");
    }
    impl_->names[index] = name;
}

} // namespace slideio::processor
