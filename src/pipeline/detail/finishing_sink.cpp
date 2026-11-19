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

void FinishingSink::emit(const ObjectRecord& record) {
    const Operation* producer = producerOf(record.layer());
    if (producer == nullptr) {
        target_.emit(record);
        ++emitted_;
        return;
    }
    ObjectRecord finished(record);
    finishAndPass(*producer, finished);
}

void FinishingSink::emitOwned(ObjectRecord& record) {
    const Operation* producer = producerOf(record.layer());
    if (producer == nullptr) {
        target_.emit(record);
        ++emitted_;
        return;
    }
    finishAndPass(*producer, record);
}

const Operation* FinishingSink::producerOf(LayerId layer) const {
    const auto producer = std::find_if(producers_.begin(), producers_.end(),
                                       [layer](const std::pair<LayerId, const Operation*>& p) {
                                           return p.first == layer;
                                       });
    return producer == producers_.end() ? nullptr : producer->second;
}

void FinishingSink::finishAndPass(const Operation& producer, ObjectRecord& record) {
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
        return;
    }
    target_.emit(record);
    ++emitted_;
}

void FinishingSink::flush() { target_.flush(); }

int64_t FinishingSink::emittedCount() const { return emitted_.load(); }

} // namespace slideio::processor::detail
