#pragma once

/**
 * @file pipeline_all.hpp
 * @brief Umbrella header: includes every public header of the pipeline module.
 *
 * Including this file is equivalent to including each header under
 * slideio/processor/pipeline/ individually: export.hpp, backend.hpp,
 * operation.hpp, operation_parameters.hpp, operation_catalog.hpp,
 * pipeline_spec.hpp, tile_grid.hpp, reduction.hpp, standard_measurements.hpp,
 * seam_merger.hpp, executor.hpp and pipeline.hpp. The name pipeline.hpp
 * belongs to the Pipeline class, so the umbrella is pipeline_all.hpp.
 *
 * Like every public pipeline header, it pulls in no OpenCV header.
 *
 * Thread-safety: not applicable (declarations only).
 * Requirements: UR-API-2.
 */

#include <slideio/processor/pipeline/backend.hpp>
#include <slideio/processor/pipeline/executor.hpp>
#include <slideio/processor/pipeline/export.hpp>
#include <slideio/processor/pipeline/operation.hpp>
#include <slideio/processor/pipeline/operation_catalog.hpp>
#include <slideio/processor/pipeline/operation_parameters.hpp>
#include <slideio/processor/pipeline/pipeline.hpp>
#include <slideio/processor/pipeline/pipeline_spec.hpp>
#include <slideio/processor/pipeline/reduction.hpp>
#include <slideio/processor/pipeline/seam_merger.hpp>
#include <slideio/processor/pipeline/standard_measurements.hpp>
#include <slideio/processor/pipeline/tile_grid.hpp>
