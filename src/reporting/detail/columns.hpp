// Requirements: UR-RES-9, UR-RES-15, UR-SEG-6
#pragma once

// Internal: the column list a writer works from. Not installed. resolveColumns is exported
// only with test seams on (SLIDEIO_PROCESSOR_DATA_TEST_EXPORTS): the writers call it inside
// the data library, and tests reach it through the export.

#include "reporting/detail/reporting_test_export.hpp"

#include <slideio/processor/core/ids.hpp>
#include <slideio/processor/core/measurement.hpp>
#include <slideio/processor/reporting/column_schema.hpp>

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace slideio::processor {

class Store;

/** The schema's settings and a cache of its store-independent column names. */
class ColumnSchema::Impl {
public:
    IdFormat idFormat = IdFormat::TwoColumns;
    UnitPolicy units = UnitPolicy::Calibrated;
    bool layer = true;
    bool stage = true;
    bool label = true;
    LabelPolicy labelPolicy = LabelPolicy::EffectiveSet;
    bool boundingBox = true;
    bool geometryWkt = false;
    bool parent = false;
    /** An explicit measurement: a row base name, or one stage's asserted base name. */
    struct Explicit {
        std::string stage;  ///< The asserting stage's name; empty for a row measurement.
        std::string name;   ///< The base name.
        bool operator==(const Explicit& other) const {
            return stage == other.stage && name == other.name;
        }
    };
    std::vector<Explicit> measurements;  ///< Explicit measurements, in call order.
    bool allMeasurements = false;
    std::vector<std::string> names;  ///< columnNameAt()'s names; rebuilt by every setter.
};

namespace detail {

/** What a column holds. */
enum class ColumnKind : int32_t {
    LayerId,
    AnchorId,
    ObjectId,
    Layer,
    Stage,
    Labels,
    LabelsLocal,
    Label,
    LabelLocal,
    BboxX,
    BboxY,
    BboxWidth,
    BboxHeight,
    GeometryWkt,
    ParentLayerId,
    ParentAnchorId,
    ParentId,
    Measurement,         ///< A measurement on the object row.
    AssertedMeasurement  ///< A measurement one later stage asserted on the object.
};

/** One resolved column. */
struct ResolvedColumn {
    std::string name;  ///< The header text.
    ColumnKind kind;
    /// For Measurement and AssertedMeasurement columns: the store name read. Else empty.
    std::string measurement;
    /// For AssertedMeasurement columns: the asserting stage, invalid when the schema names
    /// a stage the store lacks (every cell is then empty). Invalid for other kinds.
    StageId stage;
};

/** The ordered columns of one export against one store. */
struct ResolvedColumns {
    std::vector<ResolvedColumn> columns;
};

/** Friend of ColumnSchema: gives the detail code its settings. */
class ColumnSchemaAccess {
public:
    static const ColumnSchema::Impl& impl(const ColumnSchema& schema) noexcept;
};

/** How a writer lays the columns out. */
enum class ColumnLayout : int32_t {
    /// A table (CSV): the fixed columns, then the measurements. allMeasurements() skips a
    /// measurement named like a fixed column, which would repeat a header name.
    Table,
    /// Measurements only (GeoJSON, whose measurements are a nested object): no fixed
    /// columns, and allMeasurements() keeps a measurement named like a fixed column.
    MeasurementsOnly
};

/**
 * Resolves the schema against the measurement names the store holds: the row names
 * (ObjectStore::measurementNameAt()) and the asserted (stage, name) pairs
 * (AnnotationStore::assertedMeasurementColumnAt()). An asserted column is named
 * "<stage name>/<name>". Under allMeasurements(), a twin ("n.um", "n.um2") whose base no
 * row (or, for an assertion, no assertion of that stage) holds counts as that base, and
 * the unit policy picks its columns. It does not check that the names are distinct:
 * every writer calls requireDistinctNames() on the result before it creates its file.
 */
SLIDEIO_PROCESSOR_DATA_TEST_EXPORTS ResolvedColumns resolveColumns(
    const ColumnSchema& schema, Store& store, ColumnLayout layout = ColumnLayout::Table);

/**
 * Throws InvalidArgumentError, prefixed by `who`, when two resolved columns share a
 * name (for example a row measurement literally named "ki67/positive_fraction" beside
 * that stage's assertion). The message carries no name.
 */
SLIDEIO_PROCESSOR_DATA_TEST_EXPORTS void requireDistinctNames(const ResolvedColumns& resolved,
                                                          const char* who);

/** The column name of one stage's asserted measurement: "<stage name>/<name>". */
std::string assertedColumnName(const std::string& stageName, const std::string& name);

/** A borrowed (asserting stage id, name) key: looks a value up without copying the name. */
struct AssertedKeyView {
    int32_t stage;
    const std::string& name;
};

/** Orders (stage id, name) keys, owned or borrowed, by stage id, then name bytewise. */
struct AssertedKeyLess {
    using is_transparent = void;  ///< Lets find() take an AssertedKeyView.
    using Key = std::pair<int32_t, std::string>;
    bool operator()(const Key& a, const Key& b) const noexcept {
        return a.first != b.first ? a.first < b.first : a.second < b.second;
    }
    bool operator()(const Key& a, const AssertedKeyView& b) const noexcept {
        return a.first != b.stage ? a.first < b.stage : a.second < b.name;
    }
    bool operator()(const AssertedKeyView& a, const Key& b) const noexcept {
        return a.stage != b.first ? a.stage < b.first : a.name < b.second;
    }
};

/**
 * One object's visible asserted measurements, keyed by (asserting stage id, name).
 * Look a column up with find(AssertedKeyView{stage, name}), which copies nothing.
 */
using AssertedValues = std::map<std::pair<int32_t, std::string>, Measurement, AssertedKeyLess>;

/**
 * The value of an asserted-measurement column in `values`, or null when the object has
 * none (the column's stage is invalid, or the stage asserted no visible value).
 */
const Measurement* findAsserted(const AssertedValues& values, const ResolvedColumn& column);

/**
 * Replaces `out` with the object's visible asserted measurements, read once through
 * AnnotationStore::assertedMeasurementCount(), assertedMeasurementAt() and
 * measurementAssertedBy().
 */
void readAssertedValues(Store& store, ObjectId object, AssertedValues& out);

/** The fixed columns of the schema, in order. */
std::vector<ResolvedColumn> fixedColumns(const ColumnSchema::Impl& settings);

/** True when the name ends in ".um" or ".um2". */
bool isTwinName(const std::string& name) noexcept;

/** True when the name is one of the fixed column names, under any setting. */
bool isFixedColumnName(const std::string& name) noexcept;

} // namespace detail
} // namespace slideio::processor
