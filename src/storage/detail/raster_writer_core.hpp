// Requirements: UR-RES-7, UR-STG-4
#pragma once

// What RasterRegistry and RasterWriter share (ruling S47): the registry's set of live
// writers, and the setup the registry hands a new writer. RasterWriter::Impl is defined
// here so that RasterRegistry::writerForStage, a friend of RasterWriter, can build one;
// its core, RasterWriterCore, stays private to raster_writer.cpp.

#include "storage/detail/tile_spool.hpp"

#include <slideio/processor/core/ids.hpp>
#include <slideio/processor/storage/raster_store.hpp>

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace slideio::processor::detail {

class StoreGate;
class RasterWriterCore;

// The files a live writer owns in the raster directory: bare names, no directory.
struct LiveRasterFiles {
    std::string spoolName;
    std::string tifName;
};

// The live writers of one store: raster id -> the files its writer owns. A live writer is
// one that has not been flushed or destroyed; a raster has at most one. Shared
// (shared_ptr) by the registry and its writers, so a writer that outlives its store can
// still leave the set. The sweep (removeUnreferencedFiles) spares these files.
struct LiveRasterWriters {
    std::mutex mutex;  // guards byRaster
    std::map<int32_t, LiveRasterFiles> byRaster;
};

// Everything a new writer needs. The registry has validated the stage and raster, created
// `directory`, and entered `files` in `live` under `raster`; the writer leaves the set
// when it is flushed or abandoned.
struct RasterWriterSetup {
    std::shared_ptr<StoreGate> gate;
    std::shared_ptr<LiveRasterWriters> live;
    StageId stage;
    RasterId raster;
    std::string name;         // the raster's name, for the TIFF and for messages
    std::string description;  // its description, for the TIFF
    RasterFormat format;
    std::filesystem::path directory;
    LiveRasterFiles files;
};

} // namespace slideio::processor::detail

namespace slideio::processor {

// The body of a RasterWriter: a handle on the core that the store gate also tracks.
class RasterWriter::Impl {
public:
    // Opens the spool and returns a writer tracked on the gate. Throws IoError naming the
    // spool when it cannot be created, and std::bad_alloc. On a throw, the entry has left
    // setup.live, and the spool is deleted where it can be.
    static RasterWriterPtr open(detail::RasterWriterSetup setup);

    std::shared_ptr<detail::RasterWriterCore> core;
};

} // namespace slideio::processor
