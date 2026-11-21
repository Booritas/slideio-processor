// Requirements: UR-RES-9, UR-RES-15, UR-SEG-6
#pragma once

// Internal: the column list a writer works from. Not installed. resolveColumns is exported
// only with test seams on (SLIDEIO_PROCESSOR_DATA_TEST_EXPORTS): the writers call it inside
// the data library, and tests reach it through the export.

#include "reporting/detail/reporting_test_export.hpp"

#include <slideio/processor/reporting/column_schema.hpp>

#include <cstdint>
#include <string>
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
    std::vector<std::string> measurements;  ///< Explicit base names, in call order.
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
    Measurement
};

/** One resolved column. */
struct ResolvedColumn {
    std::string name;  ///< The header text.
    ColumnKind kind;
    std::string measurement;  ///< For Measurement columns: the store name read. Else empty.
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

/** Resolves the schema against the measurement names the store holds. */
SLIDEIO_PROCESSOR_DATA_TEST_EXPORTS ResolvedColumns resolveColumns(const ColumnSchema& schema, Store& store);

/** The fixed columns of the schema, in order. */
std::vector<ResolvedColumn> fixedColumns(const ColumnSchema::Impl& settings);

/** True when the name ends in ".um" or ".um2". */
bool isTwinName(const std::string& name) noexcept;

} // namespace detail
} // namespace slideio::processor
