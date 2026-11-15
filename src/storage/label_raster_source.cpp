#include "storage/detail/query_builder.hpp"
#include "storage/detail/sqlite.hpp"
#include "storage/detail/store_impl.hpp"

#include <slideio/processor/core/block_view.hpp>
#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/flow.hpp>
#include <slideio/processor/core/object_record.hpp>
#include <slideio/processor/storage/types.hpp>

#include <memory>
#include <string>
#include <utility>

namespace slideio::processor::detail {

namespace {

constexpr const char* kOperation = "AnnotationStore::layerRasterSource";

// The effective label (ruling S15): the first member of the effective set that
// annotation_store.cpp defines, the row label, else the smallest asserted label whose
// stage has the bound status. NULL when neither exists, which recordFromRow reads as no
// label. It takes the place of kObjectColumns' o.label_id.
constexpr const char* kRowLabelColumn = "o.label_id";
constexpr const char* kEffectiveLabelColumn =
    "COALESCE(o.label_id, (SELECT MIN(a.label_id) FROM label_assertions a JOIN stages sa ON sa.id = a.stage_id "
    "WHERE a.object_rowid = o.rowid AND sa.status = ?))";

// kObjectColumns with the row label replaced by the effective label. Its one parameter
// precedes every other in a statement that starts with these columns.
std::string effectiveColumns() {
    std::string columns = kObjectColumns;
    const std::string::size_type at = columns.find(kRowLabelColumn);
    if (at == std::string::npos) {
        throw InternalError("AnnotationStore::layerRasterSource: the object columns hold no row label");
    }
    columns.replace(at, std::char_traits<char>::length(kRowLabelColumn), kEffectiveLabelColumn);
    return columns;
}

QueryValue completeStatus() {
    QueryValue value;
    value.integer = static_cast<int64_t>(StageStatus::Complete);
    return value;
}

// The store's objects as core sees them, except that each record's label is its
// effective label. core's layer raster source runs over it, so the Label raster shares
// core's grid, query extent and drawing order. It reaches the store through the gate on
// every call, so a call after Store::close() throws IoError "store is closed".
class EffectiveLabelObjects final : public ObjectSource {
public:
    explicit EffectiveLabelObjects(std::shared_ptr<StoreGate> gate) : gate_(std::move(gate)) {}

    CursorPtr query(const ObjectQuery& query) override {
        BuiltQuery built = buildObjectQuery(query, kOperation);
        const std::string select = std::string("SELECT ") + kObjectColumns;
        if (built.rowsSql.compare(0, select.size(), select) != 0) {
            throw InternalError("AnnotationStore::layerRasterSource: unexpected object query");
        }
        built.rowsSql.replace(0, select.size(), "SELECT " + effectiveColumns());
        built.values.insert(built.values.begin(), completeStatus());
        built.countSql.clear();  // its parameters no longer line up with values
        return gate_->with(kOperation, [&](StoreImpl& store) {
            ReadLease lease = store.acquireRead(kOperation);
            return openObjectCursor(store.path(), std::move(lease), std::move(built));
        });
    }

    // The label plays no part in a count, so it is the object store's.
    int64_t count(const ObjectQuery& query) const override {
        BuiltQuery built = buildObjectQuery(query, kOperation);
        return gate_->with(kOperation, [&](StoreImpl& store) {
            if (built.hasPolygon) {
                return countWithPolygon(store.path(), store.acquireRead(kOperation), std::move(built));
            }
            return store.withReadConnection(kOperation, [&built](sqlite3* db) {
                Statement select(db, built.countSql.c_str());
                bindValues(select, built.values);
                return select.step() ? select.columnInt64(0) : int64_t(0);
            });
        });
    }

    bool get(ObjectId id, ObjectRecord& out) const override {
        return gate_->with(kOperation, [&](StoreImpl& store) {
            if (!id.isValid()) return false;
            return store.withReadConnection(kOperation, [&](sqlite3* db) {
                // The object and its measurements from one snapshot; the pool rolls the
                // transaction back if anything below throws.
                exec(db, "BEGIN");
                bool found = false;
                ObjectRecord record;
                {
                    const std::string sql = "SELECT " + effectiveColumns() +
                                            " FROM objects o JOIN stages s ON s.id = o.stage_id "
                                            "WHERE o.layer_id = ? AND o.anchor_id = ? AND s.status = ?";
                    Statement select(db, sql.c_str());
                    select.bindInt64(1, static_cast<int64_t>(StageStatus::Complete));
                    select.bindInt64(2, id.layer().value());
                    select.bindInt64(3, id.anchorPacked());
                    select.bindInt64(4, static_cast<int64_t>(StageStatus::Complete));
                    if (select.step()) {
                        Polygon geometry;
                        decodeRowGeometry(select, geometry);
                        Statement measurements(db, kMeasurementSelect);
                        record = recordFromRow(select, geometry, measurements);
                        found = true;
                    }
                }
                exec(db, "COMMIT");
                if (found) out = std::move(record);
                return found;
            });
        });
    }

private:
    std::shared_ptr<StoreGate> gate_;  // the store is borrowed (C-8) and reached through the gate
};

// core's Label source over the effective-label objects, which it owns.
class LabelRasterSource final : public TileRasterSource {
public:
    LabelRasterSource(std::shared_ptr<StoreGate> gate, LayerId layer)
        : objects_(std::move(gate)), source_(makeLayerRasterSource(objects_, layer, LayerRasterContent::Label)) {}

    void fillTile(BlockView& out) override { source_->fillTile(out); }

    DataType planeType() const override { return source_->planeType(); }

    int32_t planeCount() const override { return source_->planeCount(); }

private:
    EffectiveLabelObjects objects_;  // declared first: source_ borrows it
    TileRasterSourcePtr source_;
};

} // namespace

TileRasterSourcePtr makeLabelRasterSource(std::shared_ptr<StoreGate> gate, LayerId layer) {
    return std::make_shared<LabelRasterSource>(std::move(gate), layer);
}

} // namespace slideio::processor::detail
