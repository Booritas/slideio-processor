// Requirements: UR-API-2
#pragma once

/**
 * @file run.hpp
 * @brief Umbrella header: includes every public header of the run module.
 *
 * The run module is part of the engine library: it describes a whole slide run as a
 * pipeline file (pipeline_file.hpp, export_spec.hpp) and runs it against one slide
 * (slide_run.hpp, slide_run_result.hpp). It depends on workspace, masks,
 * pipeline and the data library, never on algorithm.
 *
 * Thread-safety: not applicable (declarations only).
 * Requirements: UR-API-2.
 */

#include <slideio/processor/run/export_spec.hpp>
#include <slideio/processor/run/pipeline_file.hpp>
#include <slideio/processor/run/slide_run.hpp>
#include <slideio/processor/run/slide_run_result.hpp>
