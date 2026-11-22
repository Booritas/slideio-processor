// Requirements: UR-IMG-4, UR-SEG-3
#include <pipeline/detail/level_table.hpp>

#include "core/detail/diagnostics.hpp"
#include <pipeline/detail/slide_cause.hpp>
#include <slideio/processor/config/configuration.hpp>
#include <slideio/processor/core/error.hpp>

#include <slideio/core/levelinfo.hpp>
#include <slideio/slideio/scene.hpp>

#include <exception>
#include <new>
#include <string>
#include <tuple>

namespace slideio::processor::detail {

std::vector<LevelDims> levelTable(int32_t reportedLevels, int64_t sceneWidth, int64_t sceneHeight,
                                  const std::function<LevelDims(int32_t)>& levelAt) {
    std::vector<LevelDims> table;
    if (reportedLevels <= 0) {
        table.push_back(LevelDims{sceneWidth, sceneHeight, 0, 0});
    } else {
        table.reserve(static_cast<size_t>(reportedLevels));
        for (int32_t level = 0; level < reportedLevels; ++level) {
            table.push_back(levelAt(level));
        }
    }
    for (size_t level = 0; level < table.size(); ++level) {
        if (table[level].width <= 0 || table[level].height <= 0) {
            throw InvalidArgumentError(
                ("LevelSelector: level " + std::to_string(level) + " of the scene has a non-positive size").c_str());
        }
    }
    return table;
}

std::vector<LevelDims> levelTable(const slideio::Scene& scene) {
    try {
        const std::tuple<int, int, int, int> rect = scene.getRect();
        return levelTable(scene.getNumZoomLevels(), std::get<2>(rect), std::get<3>(rect), [&scene](int32_t level) {
            const slideio::LevelInfo* info = scene.getLevelInfo(level);
            if (info == nullptr) {
                throw InternalError(("LevelSelector: SlideIO reported no level " + std::to_string(level)).c_str());
            }
            const slideio::Size size = info->getSize();
            const slideio::Size tile = info->getTileSize();
            return LevelDims{size.width, size.height, tile.width, tile.height};
        });
    } catch (const ProcessorError&) {
        throw;
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::exception& e) {
        ErrorContext context;
        const std::string causeText = detail::slideCauseText(e, scene);
        context.cause = causeText.c_str();
        throw InternalError("LevelSelector: SlideIO failed to report the scene's levels", context);
    }
}

void tileSizeOfLevel(const LevelDims& level, const Configuration& config, int32_t& widthOut, int32_t& heightOut) {
    if (config.tileSizeFromSlide() && level.tileWidth > 0 && level.tileHeight > 0) {
        widthOut = level.tileWidth;
        heightOut = level.tileHeight;
    } else {
        widthOut = config.tileWidth();
        heightOut = config.tileHeight();
    }
}

double scaleOfLevel(const std::vector<LevelDims>& table, int32_t level) noexcept {
    return static_cast<double>(table.front().width) / static_cast<double>(table[static_cast<size_t>(level)].width);
}

} // namespace slideio::processor::detail
