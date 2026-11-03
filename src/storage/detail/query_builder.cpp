#include "storage/detail/query_builder.hpp"

#include <slideio/processor/core/error.hpp>
#include <slideio/processor/storage/types.hpp>

#include <cmath>
#include <limits>

namespace slideio::processor::detail {

namespace {

[[noreturn]] void throwInvalid(const char* operation, const std::string& what) {
    const std::string message = std::string(operation) + ": " + what;
    throw InvalidArgumentError(message.c_str());
}

int64_t saturatingAdd(int64_t a, int64_t b) noexcept {
    if (b > 0 && a > std::numeric_limits<int64_t>::max() - b) return std::numeric_limits<int64_t>::max();
    if (b < 0 && a < std::numeric_limits<int64_t>::min() - b) return std::numeric_limits<int64_t>::min();
    return a + b;
}

// Compares a float with an int64 exactly: -1, 0 or 1 as f is below, equal to or above v.
int compareFloat(float f, int64_t v) noexcept {
    constexpr float kTwoTo63 = 9223372036854775808.0f;
    if (f >= kTwoTo63) return 1;
    if (f < -kTwoTo63) return -1;
    const float whole = std::floor(f);  // exact; within [-2^63, 2^63), so it converts exactly
    const int64_t t = static_cast<int64_t>(whole);
    if (t != v) return t < v ? -1 : 1;
    return f > whole ? 1 : 0;
}

class SqlBuilder {
public:
    void where(const char* clause) {
        sql_ += " AND ";
        sql_ += clause;
    }
    void bind(int64_t v) {
        QueryValue value;
        value.integer = v;
        values_.push_back(std::move(value));
    }
    void bindReal(double v) {
        QueryValue value;
        value.type = QueryValue::Type::Double;
        value.real = v;
        values_.push_back(std::move(value));
    }
    void bindText(const char* v) {
        QueryValue value;
        value.type = QueryValue::Type::Text;
        value.text = v;
        values_.push_back(std::move(value));
    }
    std::string& sql() { return sql_; }
    std::vector<QueryValue>& values() { return values_; }

private:
    std::string sql_;
    std::vector<QueryValue> values_;
};

void validate(const ObjectQuery& query, const char* operation) {
    const auto inRange = [](auto value, auto last) {
        return static_cast<int32_t>(value) >= 0 && static_cast<int32_t>(value) <= static_cast<int32_t>(last);
    };
    if (!inRange(query.visibilityValue(), StageVisibility::IncludeInFlight)) {
        throwInvalid(operation, "visibility " + std::to_string(static_cast<int32_t>(query.visibilityValue())) +
                                    " is not a StageVisibility");
    }
    if (!inRange(query.labelFilterValue(), LabelFilter::UnlabelledOnly)) {
        throwInvalid(operation, "label filter " + std::to_string(static_cast<int32_t>(query.labelFilterValue())) +
                                    " is not a LabelFilter");
    }
    if (!inRange(query.attachmentValue(), AttachmentFilter::UnattachedOnly)) {
        throwInvalid(operation, "attachment filter " + std::to_string(static_cast<int32_t>(query.attachmentValue())) +
                                    " is not an AttachmentFilter");
    }
    if (query.hasWithinPolygon() && !inRange(query.withinPolygonPredicate(), SpatialPredicate::ContainedBy)) {
        throwInvalid(operation, "polygon predicate " +
                                    std::to_string(static_cast<int32_t>(query.withinPolygonPredicate())) +
                                    " is not a SpatialPredicate");
    }
    if (query.hasLimit() && query.limitValue() < 0) {
        throwInvalid(operation, "the limit " + std::to_string(query.limitValue()) + " is negative");
    }
}

} // namespace

double floatDown(int64_t v) noexcept {
    float f = static_cast<float>(v);
    while (compareFloat(f, v) > 0) f = std::nextafter(f, -std::numeric_limits<float>::infinity());
    return f;
}

double floatUp(int64_t v) noexcept {
    float f = static_cast<float>(v);
    while (compareFloat(f, v) < 0) f = std::nextafter(f, std::numeric_limits<float>::infinity());
    return f;
}

BuiltQuery buildObjectQuery(const ObjectQuery& query, const char* operation) {
    validate(query, operation);
    BuiltQuery built;
    SqlBuilder b;

    const bool hasRect = query.hasIntersecting();
    const Rect64 rect = query.intersectingValue();
    const bool hasPolygon = query.hasWithinPolygon();
    const Polygon& polygon = query.withinPolygonValue();
    const bool spatial = (hasRect && !rect.isEmpty()) || (hasPolygon && !polygon.isEmpty());

    std::string from = spatial ? " FROM object_rtree r CROSS JOIN objects o ON o.rowid = r.rowid" : " FROM objects o";
    if (query.visibilityValue() == StageVisibility::CompletedOnly) from += " JOIN stages s ON s.id = o.stage_id";
    b.sql() = from + " WHERE 1";

    // An empty rectangle or polygon admits nothing.
    if ((hasRect && rect.isEmpty()) || (hasPolygon && polygon.isEmpty())) b.where("0");
    if (spatial) {
        // The R*Tree box is a superset (float bounds rounded outward); the exact int64
        // columns of objects decide.
        const Rect64 box = hasRect && !rect.isEmpty() ? rect : polygon.boundingBox();
        const int64_t right = saturatingAdd(box.x, box.width);
        const int64_t bottom = saturatingAdd(box.y, box.height);
        b.where("r.max_x >= ? AND r.min_x <= ? AND r.max_y >= ? AND r.min_y <= ?");
        b.bindReal(floatDown(box.x));
        b.bindReal(floatUp(right));
        b.bindReal(floatDown(box.y));
        b.bindReal(floatUp(bottom));
    }
    if (hasRect && !rect.isEmpty()) {
        // Half-open: the bounding box shares at least one pixel with the rectangle.
        b.where("o.min_x < ? AND o.max_x > ? AND o.min_y < ? AND o.max_y > ?");
        b.bind(saturatingAdd(rect.x, rect.width));
        b.bind(rect.x);
        b.bind(saturatingAdd(rect.y, rect.height));
        b.bind(rect.y);
    }
    if (hasPolygon && !polygon.isEmpty()) {
        // Closed: a polygon that only touches an object still meets it.
        const Rect64 box = polygon.boundingBox();
        b.where("o.min_x <= ? AND o.max_x >= ? AND o.min_y <= ? AND o.max_y >= ?");
        b.bind(box.right());
        b.bind(box.x);
        b.bind(box.bottom());
        b.bind(box.y);
        built.hasPolygon = true;
        built.polygon = polygon;
        built.predicate = query.withinPolygonPredicate();
    }
    if (query.visibilityValue() == StageVisibility::CompletedOnly) {
        b.where("s.status = ?");
        b.bind(static_cast<int64_t>(StageStatus::Complete));
    }
    if (query.hasLayer()) {
        b.where("o.layer_id = ?");
        b.bind(query.layerValue().value());
    }
    if (query.hasStage()) {
        b.where("o.stage_id = ?");
        b.bind(query.stageValue().value());
    }
    if (query.hasLabel()) {
        b.where("o.label_id = ?");
        b.bind(query.labelValue().value());
    }
    // The row label is the whole effective label set until label assertions exist.
    if (query.labelFilterValue() == LabelFilter::LabelledOnly) b.where("o.label_id IS NOT NULL");
    if (query.labelFilterValue() == LabelFilter::UnlabelledOnly) b.where("o.label_id IS NULL");
    // No parent edge exists before edges are stored: nothing is attached.
    if (query.attachmentValue() == AttachmentFilter::AttachedOnly) b.where("0");
    for (int32_t i = 0; i < query.measurementBoundCount(); ++i) {
        const MeasurementBound bound = query.measurementBoundAt(i);
        b.where(bound.isLowerBound
                    ? "EXISTS (SELECT 1 FROM measurements m JOIN measurement_names n ON n.id = m.name_id "
                      "WHERE m.object_rowid = o.rowid AND n.name = ? AND m.value >= ?)"
                    : "EXISTS (SELECT 1 FROM measurements m JOIN measurement_names n ON n.id = m.name_id "
                      "WHERE m.object_rowid = o.rowid AND n.name = ? AND m.value <= ?)");
        b.bindText(bound.name);
        b.bindReal(bound.value);
    }

    built.hasLimit = query.hasLimit();
    built.limit = query.limitValue();
    std::string limit;
    if (built.hasLimit && !built.hasPolygon) {
        limit = " LIMIT ?";
        b.bind(built.limit);
    }
    built.rowsSql = std::string("SELECT ") + kObjectColumns + b.sql() + " ORDER BY o.layer_id, o.anchor_id" + limit;
    built.countSql = "SELECT COUNT(*) FROM (SELECT o.rowid" + b.sql() + limit + ")";
    built.values = std::move(b.values());
    return built;
}

void bindValues(Statement& statement, const std::vector<QueryValue>& values) {
    int index = 1;
    for (const QueryValue& value : values) {
        switch (value.type) {
        case QueryValue::Type::Int64:
            statement.bindInt64(index, value.integer);
            break;
        case QueryValue::Type::Double:
            statement.bindDouble(index, value.real);
            break;
        case QueryValue::Type::Text:
            statement.bindText(index, value.text);
            break;
        }
        ++index;
    }
}

} // namespace slideio::processor::detail
