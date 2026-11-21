// Requirements: UR-RES-6
#include <slideio/processor/reporting/materialise.hpp>

#include <slideio/processor/core/error.hpp>
#include <slideio/processor/storage/object_store.hpp>
#include <slideio/processor/storage/store.hpp>

#include <vector>

namespace slideio::processor {

class ObjectArray::Impl {
public:
    std::vector<ObjectRecord> records;
};

ObjectArray::ObjectArray() : impl_(std::make_unique<Impl>()) {}
ObjectArray::~ObjectArray() = default;

int64_t ObjectArray::count() const noexcept { return static_cast<int64_t>(impl_->records.size()); }

const ObjectRecord& ObjectArray::at(int64_t index) const {
    if (index < 0 || index >= count()) throw InvalidArgumentError("ObjectArray::at: index out of range");
    return impl_->records[static_cast<size_t>(index)];
}

ObjectArrayPtr materialise(Store& store, const ObjectQuery& query, int64_t maxObjects) {
    if (maxObjects < 1) throw InvalidArgumentError("materialise: maxObjects must be at least 1");
    if (query.isUnfiltered()) throw InvalidArgumentError("materialise: the query is unfiltered");
    ObjectStore& objects = store.objects();
    if (objects.count(query) > maxObjects)
        throw InvalidArgumentError("materialise: the query selects more objects than maxObjects");

    ObjectArrayPtr array(new ObjectArray());
    CursorPtr cursor = objects.query(query);
    // The count above and the cursor's snapshot are separate reads, so the cap is
    // enforced again while reading.
    while (cursor->next()) {
        if (static_cast<int64_t>(array->impl_->records.size()) >= maxObjects) {
            cursor->close();
            throw InvalidArgumentError("materialise: the query selects more objects than maxObjects");
        }
        array->impl_->records.push_back(cursor->current());
    }
    cursor->close();
    return array;
}

} // namespace slideio::processor
