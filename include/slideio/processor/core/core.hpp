#pragma once

/**
 * @file core.hpp
 * @brief Umbrella header: includes every public header of the core module.
 *
 * Including this file is equivalent to including each header under
 * slideio/processor/core/ individually.
 *
 * Thread-safety: not applicable (declarations only).
 * Requirements: UR-API-2, UR-API-7.
 */

#include <slideio/processor/core/block_view.hpp>
#include <slideio/processor/core/channel_selection.hpp>
#include <slideio/processor/core/control.hpp>
#include <slideio/processor/core/data_type.hpp>
#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/export.hpp>
#include <slideio/processor/core/flow.hpp>
#include <slideio/processor/core/geojson.hpp>
#include <slideio/processor/core/geometry.hpp>
#include <slideio/processor/core/ids.hpp>
#include <slideio/processor/core/log.hpp>
#include <slideio/processor/core/measurement.hpp>
#include <slideio/processor/core/memory_budget.hpp>
#include <slideio/processor/core/object_query.hpp>
#include <slideio/processor/core/object_record.hpp>
#include <slideio/processor/core/provenance.hpp>
#include <slideio/processor/core/raster_mask.hpp>
#include <slideio/processor/core/version.hpp>
