// Requirements: UR-IMG-4, UR-SEG-3
#pragma once

#include <cstdint>
#include <functional>
#include <vector>

namespace slideio {
class Scene;
} // namespace slideio

namespace slideio::processor::detail {

/// One pyramid level as LevelSelector and defaultTileSize see it: its size and
/// its native tile size (0 on a side for none).
struct LevelDims {
    int64_t width;
    int64_t height;
    int32_t tileWidth;
    int32_t tileHeight;
};

/// The pyramid from a reported level count (ruling P10): `levelAt(L)` for each of
/// `reportedLevels` levels, or one level the size of the scene, without native
/// tiling, when the count is 0. A level with a non-positive side throws
/// InvalidArgumentError.
std::vector<LevelDims> levelTable(int32_t reportedLevels, int64_t sceneWidth, int64_t sceneHeight,
                                  const std::function<LevelDims(int32_t)>& levelAt);

/// The pyramid of a SlideIO scene, through the overload above. SlideIO failures
/// become InternalError with the cause preserved.
std::vector<LevelDims> levelTable(const slideio::Scene& scene);

/// double(width(0)) / double(width(level)); the caller has checked the level.
double scaleOfLevel(const std::vector<LevelDims>& table, int32_t level) noexcept;

} // namespace slideio::processor::detail
