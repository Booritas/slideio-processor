// Requirements: UR-IMG-4, UR-SEG-3
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace slideio::processor {
class Configuration;
} // namespace slideio::processor

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

/// The pyramid from a reported level count: `levelAt(L)` for each of
/// `reportedLevels` levels, or one level the size of the scene, without native
/// tiling, when the count is 0. A level with a non-positive side throws
/// InvalidArgumentError.
std::vector<LevelDims> levelTable(int32_t reportedLevels, int64_t sceneWidth, int64_t sceneHeight,
                                  const std::function<LevelDims(int32_t)>& levelAt);

/// The pyramid of a SlideIO scene, through the overload above. SlideIO failures
/// become InternalError with the cause preserved.
std::vector<LevelDims> levelTable(const slideio::Scene& scene);

/// The path of the file `scene` reports, or "" when it reports none or cannot say: named in
/// the messages of SlideIO failures. std::bad_alloc and ProcessorError pass through; any other
/// exception gives "".
std::string sceneFilePath(const slideio::Scene& scene);

/// `message` naming `slide` as withPath does, "<message> (<slide>)", or `message` alone when
/// `slide` is empty (a scene that reports no file).
std::string withSlidePath(const std::string& message, const std::string& slide);

/// The tile size of a level: its native tile size when the configuration
/// takes the tile size from the slide and the level has one on both sides,
/// otherwise the configured tile size.
void tileSizeOfLevel(const LevelDims& level, const Configuration& config, int32_t& widthOut, int32_t& heightOut);

/// double(width(0)) / double(width(level)); the caller has checked the level.
double scaleOfLevel(const std::vector<LevelDims>& table, int32_t level) noexcept;

} // namespace slideio::processor::detail
