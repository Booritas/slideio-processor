// Requirements: UR-STG-2, UR-STG-8, UR-STG-11
#pragma once

// What one stage reads of another's output: the one definition of "U writes what V
// reads" that both StageRegistry's dependency graph and the staleness edges derive from,
// so the two cannot drift (the staleness graph's acyclicity relies on its read edges
// being exactly the dependency graph's). Not installed.

#include <slideio/processor/core/ids.hpp>
#include <slideio/processor/workspace/stage_definition.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace slideio::processor::detail {

struct StageRef {
    int32_t id;
    const StageDefinition* definition;
};

// A name between double quotes, as every message and detail of the module shows one.
std::string quoted(const char* name);

// True when the definition writes the layer.
bool writesLayer(const StageDefinition& def, LayerId layer);

// True when `reader` reads (readRasterAt) the raster `writer` writes.
bool readsRasterOf(const StageDefinition& reader, const StageDefinition& writer);

// The writers of every layer and raster among a list of stages, built once per
// derivation so finding a stage's sources is not a scan of every pair.
class WriterIndex {
public:
    explicit WriterIndex(const std::vector<StageRef>& stages);

    // The positions, in the list, of the other stages writing what the stage at
    // `position` reads: a layer it reads (writesLayer) or the raster it reads
    // (readsRasterOf). Ascending, each once.
    std::vector<size_t> readSources(size_t position) const;
    // The positions of the stages writing a layer, ascending; empty when none does.
    const std::vector<size_t>& writersOf(LayerId layer) const;
    // Every layer some stage writes, by id, with its writers.
    const std::map<int32_t, std::vector<size_t>>& writtenLayers() const { return layers_; }

private:
    const std::vector<StageRef>& stages_;
    std::map<int32_t, std::vector<size_t>> layers_;
    std::map<int32_t, std::vector<size_t>> rasters_;
};

} // namespace slideio::processor::detail
