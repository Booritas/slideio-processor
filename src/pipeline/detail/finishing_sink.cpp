// Requirements: UR-SEG-3, UR-SEG-5, UR-SEG-7
#include <pipeline/detail/finishing_sink.hpp>

#include <slideio/processor/core/object_record.hpp>

#include <algorithm>

namespace slideio::processor::detail {

FinishingSink::FinishingSink(ObjectSink& target, std::vector<std::pair<LayerId, const Operation*>> producers,
                             double pixelSizeMicrons, int32_t level, std::atomic<int64_t>* droppedByFilter)
    : target_(target),
      producers_(std::move(producers)),
      context_{pixelSizeMicrons, level},
      droppedByFilter_(droppedByFilter) {}

void FinishingSink::emit(const ObjectRecord& record) {
    const LayerId layer = record.layer();
    const auto producer = std::find_if(producers_.begin(), producers_.end(),
                                       [layer](const std::pair<LayerId, const Operation*>& p) {
                                           return p.first == layer;
                                       });
    if (producer == producers_.end() || producer->second == nullptr) {
        target_.emit(record);
        ++emitted_;
        return;
    }
    ObjectRecord finished(record);
    if (!producer->second->finishObject(finished, context_)) {
        if (droppedByFilter_ != nullptr) {
            ++*droppedByFilter_;
        }
        return;
    }
    target_.emit(finished);
    ++emitted_;
}

void FinishingSink::flush() { target_.flush(); }

int64_t FinishingSink::emittedCount() const { return emitted_.load(); }

} // namespace slideio::processor::detail
