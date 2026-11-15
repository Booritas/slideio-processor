#pragma once

/**
 * @file storage.hpp
 * @brief Umbrella header: includes every public header of the storage module.
 *
 * Including this file is equivalent to including each header under
 * slideio/processor/storage/ individually. It currently covers types.hpp,
 * store_format.hpp, store.hpp, event_journal.hpp, stage_state_reader.hpp,
 * stage_state_writer.hpp, layer_registry.hpp, vocabulary_registry.hpp,
 * object_store.hpp, edge_store.hpp, annotation_store.hpp and geojson_import.hpp. The
 * raster part of the module will add raster_store.hpp, and this header will include it
 * too.
 *
 * Thread-safety: not applicable (declarations only).
 */

#include <slideio/processor/storage/annotation_store.hpp>
#include <slideio/processor/storage/edge_store.hpp>
#include <slideio/processor/storage/event_journal.hpp>
#include <slideio/processor/storage/geojson_import.hpp>
#include <slideio/processor/storage/layer_registry.hpp>
#include <slideio/processor/storage/object_store.hpp>
#include <slideio/processor/storage/stage_state_reader.hpp>
#include <slideio/processor/storage/stage_state_writer.hpp>
#include <slideio/processor/storage/store.hpp>
#include <slideio/processor/storage/store_format.hpp>
#include <slideio/processor/storage/types.hpp>
#include <slideio/processor/storage/vocabulary_registry.hpp>
