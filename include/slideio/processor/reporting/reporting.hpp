#pragma once

/**
 * @file reporting.hpp
 * @brief Umbrella header: includes every public header of the reporting module.
 *
 * Including this file is equivalent to including each header under
 * slideio/processor/reporting/ individually. It currently covers column_schema.hpp,
 * csv_writer.hpp, export_result.hpp, geojson_exporter.hpp and materialise.hpp.
 *
 * Thread-safety: not applicable (declarations only).
 */

#include <slideio/processor/reporting/column_schema.hpp>
#include <slideio/processor/reporting/csv_writer.hpp>
#include <slideio/processor/reporting/export_result.hpp>
#include <slideio/processor/reporting/geojson_exporter.hpp>
#include <slideio/processor/reporting/materialise.hpp>
