#include <slideio/processor/core/measurement.hpp>

#include <slideio/processor/core/error.hpp>

#include <string>
#include <utility>
#include <vector>

namespace slideio::processor {

Measurement::Measurement() noexcept : name_(""), value_(0.0), unit_(MeasurementUnit::Dimensionless) {}

Measurement::Measurement(const char* name, double value, MeasurementUnit unit) noexcept
    : name_(name), value_(value), unit_(unit) {}

const char* Measurement::name() const noexcept { return name_; }

double Measurement::value() const noexcept { return value_; }

MeasurementUnit Measurement::unit() const noexcept { return unit_; }

bool Measurement::isCalibrated() const noexcept {
    return unit_ != MeasurementUnit::Pixels && unit_ != MeasurementUnit::SquarePixels;
}

class MeasurementSet::Impl {
public:
    struct Entry {
        std::string name;
        double value;
        MeasurementUnit unit;
    };

    std::vector<Entry>::iterator findEntry(const char* name) noexcept {
        for (auto it = entries.begin(); it != entries.end(); ++it) {
            if (it->name == name) {
                return it;
            }
        }
        return entries.end();
    }

    std::vector<Entry> entries;
};

MeasurementSet::MeasurementSet() : impl_() {}

MeasurementSet::MeasurementSet(const MeasurementSet& other)
    : impl_(other.impl_ ? std::make_unique<Impl>(*other.impl_) : nullptr) {}

MeasurementSet::MeasurementSet(MeasurementSet&& other) noexcept : impl_(std::move(other.impl_)) {}

MeasurementSet& MeasurementSet::operator=(const MeasurementSet& other) {
    if (this != &other) {
        auto copy = other.impl_ ? std::make_unique<Impl>(*other.impl_) : nullptr;
        impl_ = std::move(copy);
    }
    return *this;
}

MeasurementSet& MeasurementSet::operator=(MeasurementSet&& other) noexcept {
    if (this != &other) {
        impl_ = std::move(other.impl_);
    }
    return *this;
}

MeasurementSet::~MeasurementSet() = default;

int32_t MeasurementSet::count() const noexcept {
    return impl_ ? static_cast<int32_t>(impl_->entries.size()) : 0;
}

Measurement MeasurementSet::at(int32_t index) const {
    if (index < 0 || index >= count()) {
        throw InvalidArgumentError("MeasurementSet::at: index out of range");
    }
    const auto& e = impl_->entries[static_cast<size_t>(index)];
    return Measurement(e.name.c_str(), e.value, e.unit);
}

bool MeasurementSet::find(const char* name, Measurement& out) const noexcept {
    if (!name || !impl_) {
        return false;
    }
    auto it = impl_->findEntry(name);
    if (it == impl_->entries.end()) {
        return false;
    }
    out = Measurement(it->name.c_str(), it->value, it->unit);
    return true;
}

void MeasurementSet::set(const char* name, double value, MeasurementUnit unit) {
    if (!name || !*name) {
        throw InvalidArgumentError("MeasurementSet::set: name must not be null or empty");
    }
    if (!impl_) {
        impl_ = std::make_unique<Impl>();
    }
    auto it = impl_->findEntry(name);
    if (it != impl_->entries.end()) {
        it->value = value;
        it->unit = unit;
        return;
    }
    impl_->entries.push_back(Impl::Entry{std::string(name), value, unit});
}

void MeasurementSet::remove(const char* name) noexcept {
    if (!name || !impl_) {
        return;
    }
    auto it = impl_->findEntry(name);
    if (it != impl_->entries.end()) {
        impl_->entries.erase(it);
    }
}

void MeasurementSet::clear() noexcept {
    if (impl_) {
        impl_->entries.clear();
    }
}

} // namespace slideio::processor
