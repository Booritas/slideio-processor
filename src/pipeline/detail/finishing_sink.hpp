// Requirements: UR-SEG-3, UR-SEG-5, UR-SEG-7
#pragma once

#include <slideio/processor/core/flow.hpp>
#include <slideio/processor/core/ids.hpp>
#include <slideio/processor/pipeline/operation.hpp>

#include <atomic>
#include <cstdint>
#include <utility>
#include <vector>

namespace slideio::processor::detail {

/// The run's object sink as every object reaches it: a record is handed to the
/// finishObject() of the operation producing its layer, then passed on to the
/// target unless that discards it. The tile filter and the seam merger both
/// emit into it, so an object found whole in one tile and one merged from seam
/// fragments are finished by one rule. A record on a layer no producer is
/// listed for passes unchanged, as does one whose producer's finishesObjects()
/// is false (read once, when the sink is built), and flush() is passed on to
/// the target.
/// A record is passed on with the target's emitOwned(), so it is copied at most
/// once here. emit() may be called concurrently, as far as the target and the
/// producers allow.
class FinishingSink : public ObjectSink {
public:
    /// `producers` pairs each layer with the one operation producing it; both the
    /// target and the operations are borrowed and must outlive the sink.
    /// `droppedByFilter`, when not null, counts the discarded records.
    FinishingSink(ObjectSink& target, std::vector<std::pair<LayerId, const Operation*>> producers,
                  double pixelSizeMicrons, int32_t level, std::atomic<int64_t>* droppedByFilter = nullptr);

    /// As emitKept().
    void emit(const ObjectRecord& record) override;
    /// Finishes a copy of `record` and passes the copy on, with the target's
    /// emitOwned(), unless discarded; true when the target kept it.
    /// Throws what the producer's finishObject() or the target throws.
    bool emitKept(const ObjectRecord& record) override;
    /// As emitKept(), finishing @p record itself rather than a copy of it.
    bool emitOwned(ObjectRecord&& record) override;
    void flush() override;
    /// The records the target kept.
    int64_t emittedCount() const override;

private:
    // The operation producing @p layer, or null when none is listed.
    const Operation* producerOf(LayerId layer) const;
    // Finishes @p record with @p producer and hands it to the target unless
    // discarded; true when the target kept it.
    bool finishAndPass(const Operation& producer, ObjectRecord& record);

    ObjectSink& target_;
    std::vector<std::pair<LayerId, const Operation*>> producers_;
    ObjectFinishContext context_;
    std::atomic<int64_t>* droppedByFilter_;
    std::atomic<int64_t> emitted_{0};
};

} // namespace slideio::processor::detail
