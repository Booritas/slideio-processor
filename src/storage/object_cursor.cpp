#include "storage/detail/geometry_codec.hpp"
#include "storage/detail/polygon_predicates.hpp"
#include "storage/detail/query_builder.hpp"
#include "storage/detail/sqlite.hpp"
#include "storage/detail/store_impl.hpp"

#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/flow.hpp>

#include <cstdio>
#include <optional>
#include <string>
#include <utility>

namespace slideio::processor::detail {

void decodeRowGeometry(const Statement& row, Polygon& out) {
    const int64_t anchor = row.columnInt64(2);
    char name[96];
    std::snprintf(name, sizeof(name), "object (layer %lld, anchor %lld, %lld)",
                  static_cast<long long>(row.columnInt64(1)), static_cast<long long>(anchor & 0x7FFFFFFF),
                  static_cast<long long>(anchor >> 31));
    decodePolygon(row.columnBlob(5), row.columnBytes(5), out, name);
}

ObjectRecord recordFromRow(const Statement& row, const Polygon& geometry, Statement& measurements) {
    ObjectRecord record;
    record.setLayer(LayerId(static_cast<int32_t>(row.columnInt64(1))));
    record.setGeometry(geometry);
    if (!row.columnIsNull(4)) record.setLabel(LabelId(static_cast<int32_t>(row.columnInt64(4))));
    measurements.bindInt64(1, row.columnInt64(0));
    while (measurements.step()) {
        record.measurements().set(measurements.columnText(0).c_str(), measurements.columnDouble(1),
                                  static_cast<MeasurementUnit>(measurements.columnInt64(2)));
    }
    measurements.reset();
    return ObjectRecord::restored(record, StageId(static_cast<int32_t>(row.columnInt64(3))));
}

namespace {

// Runs a built query on a leased connection inside one read transaction, applying the
// polygon predicate and the limit to the rows SQL returns.
class ObjectCursorImpl final : public Cursor {
public:
    ObjectCursorImpl(std::string path, ReadLease&& lease, BuiltQuery&& built)
        : path_(std::move(path)), lease_(std::move(lease)), built_(std::move(built)) {
        sqlite3* db = lease_->get();
        // One read transaction for the cursor's lifetime: one snapshot, taken now by the
        // first read. The pool rolls it back when the lease returns.
        exec(db, "BEGIN");
        Statement(db, "SELECT 1 FROM meta LIMIT 1").step();
        select_.emplace(db, built_.rowsSql.c_str());
        bindValues(*select_, built_.values);
        measurements_.emplace(db, kMeasurementSelect);
    }

    bool next() override {
        checkUsable("Cursor::next");
        if (!advance()) return false;
        current_ = recordFromRow(*select_, geometry_, *measurements_);
        ++position_;
        return true;
    }

    const ObjectRecord& current() const override { return current_; }
    int64_t position() const override { return position_; }

    void close() override {
        measurements_.reset();
        select_.reset();
        lease_.reset();  // the pool rolls back the read transaction
    }

    // Steps to the next row that passes the polygon predicate and the limit, leaving its
    // geometry in geometry_. Ends the read transaction once the rows run out.
    bool advance() {
        if (done_) return false;
        if (!built_.hasLimit || matched_ < built_.limit) {
            while (select_->step()) {
                decodeRowGeometry(*select_, geometry_);
                if (built_.hasPolygon && !passesPolygon()) continue;
                ++matched_;
                return true;
            }
        }
        done_ = true;
        measurements_.reset();
        select_.reset();
        exec(lease_->get(), "COMMIT");
        return false;
    }

    void checkUsable(const char* operation) const {
        if (!lease_) {
            const std::string message = std::string(operation) + ": the cursor is closed ('" + path_ + "')";
            throw IoError(message.c_str());
        }
        lease_->checkStoreOpen(operation, path_);
    }

private:
    bool passesPolygon() const {
        return built_.predicate == SpatialPredicate::ContainedBy ? polygonContainedBy(geometry_, built_.polygon)
                                                                 : polygonsIntersect(geometry_, built_.polygon);
    }

    // Declaration order is destruction order in reverse: statements finalise before the
    // lease returns the connection.
    std::string path_;
    std::optional<ReadLease> lease_;
    BuiltQuery built_;
    std::optional<Statement> select_;
    std::optional<Statement> measurements_;
    Polygon geometry_;
    ObjectRecord current_;
    int64_t position_ = -1;
    int64_t matched_ = 0;
    bool done_ = false;
};

} // namespace

CursorPtr openObjectCursor(std::string path, ReadLease&& lease, BuiltQuery&& built) {
    return std::make_shared<ObjectCursorImpl>(std::move(path), std::move(lease), std::move(built));
}

int64_t countWithPolygon(std::string path, ReadLease&& lease, BuiltQuery&& built) {
    ObjectCursorImpl cursor(std::move(path), std::move(lease), std::move(built));
    int64_t count = 0;
    while (cursor.advance()) ++count;
    return count;
}

} // namespace slideio::processor::detail
