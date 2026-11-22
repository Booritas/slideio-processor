// Requirements: UR-API-2, UR-STG-4, UR-RES-9
#include <slideio/processor/run/slide_run_result.hpp>

#include <slideio/processor/core/error.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace slideio::processor {

class SlideRunResult::Impl {
public:
    struct Stage {
        std::string name;
        bool ran = false;
        int64_t objectsWritten = 0;
        int64_t truncatedObjects = 0;
    };
    struct Export {
        std::string file;
        int64_t bytes = 0;
    };
    std::vector<Stage> stages;
    std::vector<Export> exports;
};

namespace {

// Checks an index against a count; a moved-from result has count 0.
size_t checked(int32_t index, int32_t count, const char* what) {
    if (index < 0 || index >= count) {
        throw InvalidArgumentError((std::string("SlideRunResult: the ") + what + " index is out of range").c_str());
    }
    return static_cast<size_t>(index);
}

} // namespace

SlideRunResult::SlideRunResult() : impl_(std::make_unique<Impl>()) {}

SlideRunResult::SlideRunResult(const SlideRunResult& other)
    : impl_(other.impl_ ? std::make_unique<Impl>(*other.impl_) : std::make_unique<Impl>()) {}

SlideRunResult::SlideRunResult(SlideRunResult&& other) noexcept = default;

SlideRunResult& SlideRunResult::operator=(const SlideRunResult& other) {
    if (this != &other) {
        SlideRunResult copy(other);
        impl_ = std::move(copy.impl_);
    }
    return *this;
}

SlideRunResult& SlideRunResult::operator=(SlideRunResult&& other) noexcept = default;

SlideRunResult::~SlideRunResult() = default;

int32_t SlideRunResult::stageCount() const noexcept {
    return impl_ ? static_cast<int32_t>(impl_->stages.size()) : 0;
}

const char* SlideRunResult::stageName(int32_t index) const {
    return impl_->stages[checked(index, stageCount(), "stage")].name.c_str();
}

bool SlideRunResult::stageRan(int32_t index) const { return impl_->stages[checked(index, stageCount(), "stage")].ran; }

int64_t SlideRunResult::stageObjectsWritten(int32_t index) const {
    return impl_->stages[checked(index, stageCount(), "stage")].objectsWritten;
}

int64_t SlideRunResult::stageTruncatedObjects(int32_t index) const {
    return impl_->stages[checked(index, stageCount(), "stage")].truncatedObjects;
}

int32_t SlideRunResult::exportCount() const noexcept {
    return impl_ ? static_cast<int32_t>(impl_->exports.size()) : 0;
}

const char* SlideRunResult::exportFile(int32_t index) const {
    return impl_->exports[checked(index, exportCount(), "export")].file.c_str();
}

int64_t SlideRunResult::exportBytes(int32_t index) const {
    return impl_->exports[checked(index, exportCount(), "export")].bytes;
}

void SlideRunResult::addStage(const char* name, bool ran, int64_t objectsWritten, int64_t truncatedObjects) {
    if (!impl_) impl_ = std::make_unique<Impl>();
    impl_->stages.push_back(Impl::Stage{name, ran, objectsWritten, truncatedObjects});
}

void SlideRunResult::addExport(const char* file, int64_t bytes) {
    if (!impl_) impl_ = std::make_unique<Impl>();
    impl_->exports.push_back(Impl::Export{file, bytes});
}

} // namespace slideio::processor
