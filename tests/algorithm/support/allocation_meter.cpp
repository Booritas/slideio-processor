// Requirements: UR-EXE-2
#include <algorithm/support/allocation_meter.hpp>

#include <algorithm/support/harness.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <functional>
#include <new>
#include <stdexcept>
#include <unordered_map>
#include <utility>

#if defined(_WIN32)
#include <malloc.h>
#endif

// Every block this executable allocates stays a plain malloc (or aligned
// malloc) block, exactly as the runtime's own operator new makes it: blocks
// cross module boundaries in both directions (a string a shared library returns
// is freed here, and a block allocated here may be freed by another module's
// operator delete), so no block may carry a header the other modules do not
// know. The size of each counted block is kept instead in the active meter's
// own table, which allocates through malloc and is therefore never counted.

namespace slideio::processor::testing_support {

namespace {

template <typename T>
struct MallocAllocator {
    using value_type = T;
    MallocAllocator() = default;
    template <typename U>
    MallocAllocator(const MallocAllocator<U>&) noexcept {}
    T* allocate(std::size_t n) {
        void* p = std::malloc(n * sizeof(T));
        if (p == nullptr) {
            throw std::bad_alloc();
        }
        return static_cast<T*>(p);
    }
    void deallocate(T* p, std::size_t) noexcept { std::free(p); }
    template <typename U>
    bool operator==(const MallocAllocator<U>&) const noexcept {
        return true;
    }
    template <typename U>
    bool operator!=(const MallocAllocator<U>&) const noexcept {
        return false;
    }
};

} // namespace

/// The counts of one meter and the sizes of the blocks it has counted and not
/// yet seen freed.
class AllocationLedger {
public:
    void record(const void* block, std::size_t size) {
        blocks_.emplace(block, size);
        live_ += static_cast<int64_t>(size);
        peak_ = std::max(peak_, live_);
    }
    void release(const void* block) noexcept {
        const auto it = blocks_.find(block);
        if (it != blocks_.end()) {
            live_ -= static_cast<int64_t>(it->second);
            blocks_.erase(it);
        }
    }
    int64_t live() const { return live_; }
    int64_t peak() const { return peak_; }

private:
    std::unordered_map<const void*, std::size_t, std::hash<const void*>, std::equal_to<const void*>,
                       MallocAllocator<std::pair<const void* const, std::size_t>>>
        blocks_;
    int64_t live_ = 0;
    int64_t peak_ = 0;
};

namespace {

// The meter active on this thread, if any. A pointer is constant-initialised,
// so operator new may read it at any time, during start-up included.
thread_local AllocationLedger* tActive = nullptr;

void* rawAllocate(std::size_t size, std::size_t alignment) {
    if (alignment == 0) {
        return std::malloc(size);
    }
#if defined(_WIN32)
    return _aligned_malloc(size, alignment);
#else
    void* p = nullptr;
    return posix_memalign(&p, std::max(alignment, sizeof(void*)), size) == 0 ? p : nullptr;
#endif
}

void rawFree(void* block, std::size_t alignment) noexcept {
#if defined(_WIN32)
    if (alignment != 0) {
        _aligned_free(block);
        return;
    }
#else
    (void)alignment;
#endif
    std::free(block);
}

// The throwing allocation: retries through the new-handler as the standard
// operator new does, then counts the block for the active meter.
void* allocate(std::size_t size, std::size_t alignment) {
    const std::size_t bytes = size == 0 ? 1 : size;
    void* block = nullptr;
    for (;;) {
        block = rawAllocate(bytes, alignment);
        if (block != nullptr) {
            break;
        }
        const std::new_handler handler = std::get_new_handler();
        if (handler == nullptr) {
            throw std::bad_alloc();
        }
        handler();
    }
    if (AllocationLedger* ledger = tActive) {
        try {
            ledger->record(block, size);
        } catch (...) {
            rawFree(block, alignment);
            throw;
        }
    }
    return block;
}

void* allocateNoThrow(std::size_t size, std::size_t alignment) noexcept {
    try {
        return allocate(size, alignment);
    } catch (...) {
        return nullptr;
    }
}

void release(void* block, std::size_t alignment) noexcept {
    if (block == nullptr) {
        return;
    }
    if (AllocationLedger* ledger = tActive) {
        ledger->release(block);
    }
    rawFree(block, alignment);
}

std::size_t alignmentOf(std::align_val_t alignment) { return static_cast<std::size_t>(alignment); }

} // namespace

AllocationMeter::AllocationMeter() : ledger_(nullptr) {
    if (tActive != nullptr) {
        throw std::logic_error("AllocationMeter: a meter is already active on this thread");
    }
    ledger_ = new AllocationLedger();
    tActive = ledger_;
}

AllocationMeter::~AllocationMeter() {
    tActive = nullptr;
    delete ledger_;
}

int64_t AllocationMeter::peakBytes() const { return ledger_->peak(); }

int64_t AllocationMeter::liveBytes() const { return ledger_->live(); }

int64_t measureApplyPeak(Operation& op, const std::vector<DataType>& inputTypes,
                         const std::vector<int32_t>& sourceChannels, int64_t width, int64_t height,
                         const std::function<double(int32_t plane, int64_t x, int64_t y)>& pixel) {
    int64_t peak = 0;
    applyDirectAround(op, inputTypes, sourceChannels, width, height, pixel, [&peak](const ApplyCall& apply) {
        AllocationMeter meter;
        apply();
        peak = std::max(peak, meter.peakBytes());
    });
    return peak;
}

} // namespace slideio::processor::testing_support

namespace ts = slideio::processor::testing_support;

// The replaceable global allocation functions, every form.

void* operator new(std::size_t size) { return ts::allocate(size, 0); }
void* operator new[](std::size_t size) { return ts::allocate(size, 0); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept { return ts::allocateNoThrow(size, 0); }
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept { return ts::allocateNoThrow(size, 0); }
void* operator new(std::size_t size, std::align_val_t alignment) {
    return ts::allocate(size, ts::alignmentOf(alignment));
}
void* operator new[](std::size_t size, std::align_val_t alignment) {
    return ts::allocate(size, ts::alignmentOf(alignment));
}
void* operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    return ts::allocateNoThrow(size, ts::alignmentOf(alignment));
}
void* operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    return ts::allocateNoThrow(size, ts::alignmentOf(alignment));
}

void operator delete(void* block) noexcept { ts::release(block, 0); }
void operator delete[](void* block) noexcept { ts::release(block, 0); }
void operator delete(void* block, const std::nothrow_t&) noexcept { ts::release(block, 0); }
void operator delete[](void* block, const std::nothrow_t&) noexcept { ts::release(block, 0); }
void operator delete(void* block, std::size_t) noexcept { ts::release(block, 0); }
void operator delete[](void* block, std::size_t) noexcept { ts::release(block, 0); }
void operator delete(void* block, std::align_val_t alignment) noexcept {
    ts::release(block, ts::alignmentOf(alignment));
}
void operator delete[](void* block, std::align_val_t alignment) noexcept {
    ts::release(block, ts::alignmentOf(alignment));
}
void operator delete(void* block, std::size_t, std::align_val_t alignment) noexcept {
    ts::release(block, ts::alignmentOf(alignment));
}
void operator delete[](void* block, std::size_t, std::align_val_t alignment) noexcept {
    ts::release(block, ts::alignmentOf(alignment));
}
void operator delete(void* block, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    ts::release(block, ts::alignmentOf(alignment));
}
void operator delete[](void* block, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    ts::release(block, ts::alignmentOf(alignment));
}
