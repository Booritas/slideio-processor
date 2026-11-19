// Requirements: UR-SEG-3, UR-SEG-5, UR-SEG-7
#include <pipeline/detail/finishing_sink.hpp>

#include <slideio/processor/core/object_record.hpp>

#include <algorithm>
#include <cassert>

namespace slideio::processor::detail {

FinishingSink::FinishingSink(ObjectSink& target, std::vector<std::pair<LayerId, const Operation*>> producers,
                             double pixelSizeMicrons, int32_t level, std::atomic<int64_t>* droppedByFilter)
    : target_(target),
      producers_(std::move(producers)),
      context_{pixelSizeMicrons, level},
      droppedByFilter_(droppedByFilter) {}

void FinishingSink::emit(const ObjectRecord& record) { emitKept(record); }

bool FinishingSink::emitKept(const ObjectRecord& record) {
    const Operation* producer = producerOf(record.layer());
    if (producer == nullptr) {
        const bool kept = target_.emitKept(record);
        if (kept) {
            ++emitted_;
        }
        return kept;
    }
    ObjectRecord finished(record);
    return finishAndPass(*producer, finished);
}

bool FinishingSink::emitOwned(ObjectRecord&& record) {
    const Operation* producer = producerOf(record.layer());
    if (producer == nullptr) {
        const bool kept = target_.emitOwned(std::move(record));
        if (kept) {
            ++emitted_;
        }
        return kept;
    }
    return finishAndPass(*producer, record);
}

const Operation* FinishingSink::producerOf(LayerId layer) const {
    const auto producer = std::find_if(producers_.begin(), producers_.end(),
                                       [layer](const std::pair<LayerId, const Operation*>& p) {
                                           return p.first == layer;
                                       });
    return producer == producers_.end() ? nullptr : producer->second;
}

bool FinishingSink::finishAndPass(const Operation& producer, ObjectRecord& record) {
#ifndef NDEBUG
    const LayerId layer = record.layer();
    const Point64 anchor = record.geometry().anchor();
#endif
    const bool kept = producer.finishObject(record, context_);
    // Finishing must not move an object: its layer and anchor make its id.
    assert(record.layer() == layer);
    assert(record.geometry().anchor() == anchor);
    if (!kept) {
        if (droppedByFilter_ != nullptr) {
            ++*droppedByFilter_;
        }
        return false;
    }
    const bool passed = target_.emitOwned(std::move(record));
    if (passed) {
        ++emitted_;
    }
    return passed;
}

void FinishingSink::flush() { target_.flush(); }

int64_t FinishingSink::emittedCount() const { return emitted_.load(); }

} // namespace slideio::processor::detail
