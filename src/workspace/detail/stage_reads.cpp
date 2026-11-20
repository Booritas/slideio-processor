// Requirements: UR-STG-2, UR-STG-8, UR-STG-11
#include <workspace/detail/stage_reads.hpp>

#include <algorithm>

namespace slideio::processor::detail {

std::string quoted(const char* name) { return std::string("\"") + name + "\""; }

bool writesLayer(const StageDefinition& def, LayerId layer) {
    for (int32_t i = 0; i < def.writtenLayerCount(); ++i) {
        if (def.writtenLayerAt(i) == layer) return true;
    }
    return false;
}

bool readsRasterOf(const StageDefinition& reader, const StageDefinition& writer) {
    if (!writer.writesAnyRaster()) return false;
    for (int32_t i = 0; i < reader.readRasterCount(); ++i) {
        if (reader.readRasterAt(i) == writer.writtenRaster()) return true;
    }
    return false;
}

WriterIndex::WriterIndex(const std::vector<StageRef>& stages) : stages_(stages) {
    for (size_t p = 0; p < stages.size(); ++p) {
        const StageDefinition& def = *stages[p].definition;
        for (int32_t i = 0; i < def.writtenLayerCount(); ++i) {
            std::vector<size_t>& writers = layers_[def.writtenLayerAt(i).value()];
            if (writers.empty() || writers.back() != p) writers.push_back(p);
        }
        if (def.writesAnyRaster()) rasters_[def.writtenRaster().value()].push_back(p);
    }
}

std::vector<size_t> WriterIndex::readSources(size_t position) const {
    const StageDefinition& reader = *stages_[position].definition;
    std::vector<size_t> result;
    const auto add = [&](const std::map<int32_t, std::vector<size_t>>& index, int32_t key) {
        const auto found = index.find(key);
        if (found != index.end()) result.insert(result.end(), found->second.begin(), found->second.end());
    };
    for (int32_t i = 0; i < reader.readLayerCount(); ++i) add(layers_, reader.readLayerAt(i).value());
    for (int32_t i = 0; i < reader.readRasterCount(); ++i) add(rasters_, reader.readRasterAt(i).value());
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    result.erase(std::remove(result.begin(), result.end(), position), result.end());
    return result;
}

const std::vector<size_t>& WriterIndex::writersOf(LayerId layer) const {
    static const std::vector<size_t> none;
    const auto found = layers_.find(layer.value());
    return found != layers_.end() ? found->second : none;
}

} // namespace slideio::processor::detail
