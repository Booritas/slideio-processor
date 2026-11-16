// Requirements: UR-PIX-5, UR-QA-3, UR-IMG-2
#include <slideio/processor/pipeline/reduction.hpp>

#include <slideio/processor/core/error.hpp>

#include <pipeline/detail/half.hpp>

#include <atomic>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace slideio::processor {

Reduction::Reduction() = default;
Reduction::Reduction(const Reduction&) = default;
Reduction& Reduction::operator=(const Reduction&) = default;
Reduction::~Reduction() = default;

class Histogram64::Impl {
public:
    Impl(int32_t count, double lo, double hi)
        : binCount(count), minValue(lo), maxValue(hi), bins(new std::atomic<int64_t>[static_cast<size_t>(count)]) {
        for (int32_t i = 0; i < count; ++i) {
            bins[i].store(0, std::memory_order_relaxed);
        }
    }
    Impl(const Impl& o) : Impl(o.binCount, o.minValue, o.maxValue) {
        for (int32_t i = 0; i < binCount; ++i) {
            bins[i].store(o.bins[i].load(std::memory_order_relaxed), std::memory_order_relaxed);
        }
    }

    int32_t binCount;
    double minValue;
    double maxValue;
    std::unique_ptr<std::atomic<int64_t>[]> bins;
};

namespace {

[[noreturn]] void fail(const std::string& message) {
    throw InvalidArgumentError(message.c_str());
}

template <typename T>
double load(const unsigned char* p) {
    T v;
    std::memcpy(&v, p, sizeof(T));
    return static_cast<double>(v);
}

double loadHalf(const unsigned char* p) {
    uint16_t v;
    std::memcpy(&v, p, sizeof(v));
    return detail::halfToDouble(v);
}

using Loader = double (*)(const unsigned char*);

Loader loaderFor(DataType type) {
    switch (type) {
    case DataType::Byte: return load<uint8_t>;
    case DataType::Int8: return load<int8_t>;
    case DataType::UInt16: return load<uint16_t>;
    case DataType::Int16: return load<int16_t>;
    case DataType::UInt32: return load<uint32_t>;
    case DataType::Int32: return load<int32_t>;
    case DataType::UInt64: return load<uint64_t>;
    case DataType::Int64: return load<int64_t>;
    case DataType::Float16: return loadHalf;
    case DataType::Float32: return load<float>;
    case DataType::Float64: return load<double>;
    default: return nullptr;
    }
}

} // namespace

Histogram64::Histogram64(int32_t binCount, double minValue, double maxValue) {
    if (binCount < 1) {
        fail("Histogram64: binCount must be at least 1");
    }
    if (!std::isfinite(minValue) || !std::isfinite(maxValue)) {
        fail("Histogram64: minValue and maxValue must be finite");
    }
    if (!(minValue < maxValue)) {
        fail("Histogram64: minValue must be less than maxValue");
    }
    if (!std::isfinite(maxValue - minValue)) {
        // Finite bounds whose span overflows (e.g. -1e308..1e308) would bin through inf/inf = NaN.
        fail("Histogram64: the span maxValue - minValue must be finite");
    }
    impl_ = std::make_unique<Impl>(binCount, minValue, maxValue);
}

Histogram64::Histogram64(const Histogram64& other)
    : Reduction(other), impl_(other.impl_ ? std::make_unique<Impl>(*other.impl_) : nullptr) {}

Histogram64::Histogram64(Histogram64&& other) noexcept : Reduction(other), impl_(std::move(other.impl_)) {}

Histogram64& Histogram64::operator=(const Histogram64& other) {
    if (this != &other) {
        auto copy = other.impl_ ? std::make_unique<Impl>(*other.impl_) : nullptr;
        impl_ = std::move(copy);
    }
    return *this;
}

Histogram64& Histogram64::operator=(Histogram64&& other) noexcept {
    if (this != &other) {
        impl_ = std::move(other.impl_);
    }
    return *this;
}

Histogram64::~Histogram64() = default;

void Histogram64::accumulate(const BlockView& block, int64_t /*tileIndex*/) {
    if (!impl_) {
        fail("Histogram64::accumulate: the histogram was moved from");
    }
    if (block.isNull()) {
        fail("Histogram64::accumulate: the block is null");
    }
    const int32_t n = impl_->binCount;
    const double lo = impl_->minValue;
    const double hi = impl_->maxValue;
    const double range = hi - lo;
    // Counts are gathered locally and added once, so the shared atomics see one
    // update per bin per call, not one per sample.
    std::vector<int64_t> local(static_cast<size_t>(n), 0);
    for (int32_t plane = 0; plane < block.planeCount(); ++plane) {
        const DataType type = block.planeType(plane);
        const Loader read = loaderFor(type);
        if (read == nullptr) {
            fail(std::string("Histogram64::accumulate: unsupported plane type ") + dataTypeName(type));
        }
        const auto* base = static_cast<const unsigned char*>(block.planeData(plane));
        const int64_t stride = block.planeRowStrideBytes(plane);
        const int64_t size = dataTypeSizeBytes(type);
        for (int64_t y = 0; y < block.height(); ++y) {
            const unsigned char* p = base + y * stride;
            for (int64_t x = 0; x < block.width(); ++x, p += size) {
                const double v = read(p);
                if (!(v >= lo && v <= hi)) { // also rejects NaN
                    continue;
                }
                auto index = static_cast<int64_t>(std::floor((v - lo) / range * n));
                if (index >= n) {
                    index = n - 1;
                }
                if (index < 0) {
                    index = 0;
                }
                ++local[static_cast<size_t>(index)];
            }
        }
    }
    for (int32_t i = 0; i < n; ++i) {
        if (local[static_cast<size_t>(i)] != 0) {
            impl_->bins[i].fetch_add(local[static_cast<size_t>(i)], std::memory_order_relaxed);
        }
    }
}

void Histogram64::combine(const Reduction& other) {
    const auto* o = dynamic_cast<const Histogram64*>(&other);
    if (o == nullptr) {
        fail("Histogram64::combine: the other reduction is not a Histogram64");
    }
    if (!impl_ || !o->impl_) {
        fail("Histogram64::combine: a histogram was moved from");
    }
    if (o->impl_->binCount != impl_->binCount || o->impl_->minValue != impl_->minValue ||
        o->impl_->maxValue != impl_->maxValue) {
        fail("Histogram64::combine: binCount, minValue and maxValue must match");
    }
    for (int32_t i = 0; i < impl_->binCount; ++i) {
        impl_->bins[i].fetch_add(o->impl_->bins[i].load(std::memory_order_relaxed), std::memory_order_relaxed);
    }
}

bool Histogram64::isOrderIndependent() const {
    return true;
}

int64_t Histogram64::bin(int32_t index) const {
    if (!impl_) {
        fail("Histogram64::bin: the histogram was moved from");
    }
    if (index < 0 || index >= impl_->binCount) {
        fail("Histogram64::bin: index " + std::to_string(index) + " is outside [0, " +
             std::to_string(impl_->binCount) + ")");
    }
    return impl_->bins[index].load(std::memory_order_relaxed);
}

int64_t Histogram64::totalCount() const {
    if (!impl_) {
        fail("Histogram64::totalCount: the histogram was moved from");
    }
    int64_t total = 0;
    for (int32_t i = 0; i < impl_->binCount; ++i) {
        total += impl_->bins[i].load(std::memory_order_relaxed);
    }
    return total;
}

} // namespace slideio::processor
