// Requirements: UR-RES-2, UR-RES-3, UR-STG-5, UR-OBS-4, UR-QA-3
#pragma once

// The read path of ObjectStore (ruling S10). buildObjectQuery turns an ObjectQuery into
// one SQL statement; object_cursor.cpp runs it, applying the exact polygon predicate and
// a polygon query's limit in its loop, and rebuilds records from rows.

#include "storage/detail/sqlite.hpp"
#include "storage/detail/store_impl.hpp"

#include <slideio/processor/core/flow.hpp>
#include <slideio/processor/core/geometry.hpp>
#include <slideio/processor/core/object_query.hpp>
#include <slideio/processor/core/object_record.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace slideio::processor::detail {

// The columns every object read selects, in this order, from `objects o`.
inline constexpr const char* kObjectColumns = "o.rowid, o.layer_id, o.anchor_id, o.stage_id, o.label_id, o.geometry";

// Reads one object's measurements, in their original order; bind the object rowid at 1.
inline constexpr const char* kMeasurementSelect =
    "SELECT n.name, m.value, m.unit FROM measurements m JOIN measurement_names n ON n.id = m.name_id "
    "WHERE m.object_rowid = ? ORDER BY m.ordinal";

// One parameter of a built statement.
struct QueryValue {
    enum class Type { Int64, Double, Text };
    Type type = Type::Int64;
    int64_t integer = 0;
    double real = 0.0;
    std::string text;
};

// An ObjectQuery as SQL, plus the filters SQL cannot apply exactly.
struct BuiltQuery {
    // SELECT kObjectColumns ... ORDER BY o.layer_id, o.anchor_id, with LIMIT when the
    // query has a limit and no polygon filter.
    std::string rowsSql;
    // SELECT COUNT(*) over the same rows. Exact only when hasPolygon is false.
    std::string countSql;
    std::vector<QueryValue> values;  // bound in order, to either statement
    bool hasPolygon = false;
    Polygon polygon;
    SpatialPredicate predicate = SpatialPredicate::Intersects;
    bool hasLimit = false;  // applied in the loop as well; with a polygon, only there
    int64_t limit = 0;
};

// Builds the statement for `query`. Throws InvalidArgumentError, naming `operation`, for
// a negative limit or a value outside its enumeration; std::bad_alloc.
BuiltQuery buildObjectQuery(const ObjectQuery& query, const char* operation);

// Binds `values` to `statement` from parameter 1. Throws IoError.
void bindValues(Statement& statement, const std::vector<QueryValue>& values);

// The largest float not above v, and the smallest float not below it, as doubles: the
// R*Tree stores float32, so boxes are rounded outward and queries widened, never narrowed.
double floatDown(int64_t v) noexcept;
double floatUp(int64_t v) noexcept;

// ---- Defined in object_cursor.cpp ---------------------------------------------------

// Decodes the geometry of the row `row` is on (kObjectColumns). Throws InternalError
// naming the object when the blob is corrupt; std::bad_alloc.
void decodeRowGeometry(const Statement& row, Polygon& out);

// Rebuilds the record of the row `row` is on (kObjectColumns) from its decoded geometry,
// with its measurements read through `measurements` (kMeasurementSelect, on the same
// connection) and its stage stamped by ObjectRecord::restored. Throws IoError,
// std::bad_alloc.
ObjectRecord recordFromRow(const Statement& row, const Polygon& geometry, Statement& measurements);

// A cursor over `built` on `lease`, holding one read transaction, opened here, for its
// lifetime. `path` names the store in errors. Throws IoError, std::bad_alloc.
CursorPtr openObjectCursor(std::string path, ReadLease&& lease, BuiltQuery&& built);

// The number of rows `built` yields with its polygon predicate and limit applied, read in
// one read transaction on `lease`. Throws IoError, InternalError, std::bad_alloc.
int64_t countWithPolygon(std::string path, ReadLease&& lease, BuiltQuery&& built);

} // namespace slideio::processor::detail
