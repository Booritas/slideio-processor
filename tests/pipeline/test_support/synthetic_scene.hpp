// Requirements: UR-IMG-1, UR-IMG-2, UR-IMG-9
#pragma once

// A computed scene for pipeline tests: a pyramid of given level sizes whose
// pixel values come from a function, read through SlideIO's ISceneSource and
// makeScene exactly as a decoded slide would be. Public headers only.

#include <slideio/core/slideio_enums.hpp>
#include <slideio/slideio/scene.hpp>
#include <slideio/slideio/scenesource.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace slideio::processor::testing_support {

/// One pyramid level: its size in level pixels and its native tile size (0 x 0 for none).
struct SyntheticLevel {
    int64_t width;
    int64_t height;
    int32_t tileWidth;
    int32_t tileHeight;
};

/// The value of channel `channel` at level pixel (x, y), on the level being read.
/// Must be thread-safe: SyntheticSource declares concurrent reads, so SlideIO may
/// call it from several threads at once. Values outside the channel type's range
/// saturate; NaN is written as 0 to an integer channel.
using PixelFunction = std::function<double(int, int64_t, int64_t)>;

/// Serves every level directly (readLevelBlock returns true) and level 0 through
/// readBlock. Samples are written interleaved in the requested channel order, cast
/// to the channel's type; pixels outside the level are background zero. A block
/// size different from the rect samples nearest-neighbour. Concurrent reads are
/// safe; readCount() counts every read either path serves.
class SyntheticSource : public slideio::ISceneSource {
public:
    /// Level 0 first, each level no larger than the one before; at least one
    /// channel. Throws std::invalid_argument otherwise.
    SyntheticSource(std::vector<SyntheticLevel> levels, std::vector<slideio::DataType> channelTypes,
                    PixelFunction pixel);

    void readBlock(const slideio::Rect& sceneRect, const slideio::Size& blockSize,
                   const std::vector<int>& channelIndices, int zSlice, int tFrame, void* buffer,
                   size_t bufferSize) override;
    bool readLevelBlock(int level, const slideio::Rect& levelRect, const slideio::Size& blockSize,
                        const std::vector<int>& channelIndices, int zSlice, int tFrame, void* buffer,
                        size_t bufferSize) override;
    bool supportsConcurrentReads() const override;

    /// The metadata makeScene needs: rect, channel types and the pyramid, the
    /// scale of level L being width(L) / width(0).
    slideio::SceneMetadata metadata() const;

    /// Reads served so far, by either path.
    int64_t readCount() const;

    /// The level of every read served so far, in arrival order.
    std::vector<int> readLevels() const;

    /// Makes every later read throw std::runtime_error(what), before it is counted.
    void failReadsWith(const std::string& what);

    /// The resolution, in metres per pixel, the scene made from metadata() reports
    /// at level 0; (0, 0), unknown, by default. Set it before the scene is made.
    void setResolution(double x, double y);

private:
    void fill(int level, const slideio::Rect& levelRect, const slideio::Size& blockSize,
              const std::vector<int>& channelIndices, void* buffer, size_t bufferSize);

    std::vector<SyntheticLevel> levels_;
    std::vector<slideio::DataType> channelTypes_;
    PixelFunction pixel_;
    std::atomic<int64_t> reads_{0};
    mutable std::mutex mutex_;
    std::vector<int> readLevels_;
    std::string failure_;
    double resolutionX_ = 0.0;
    double resolutionY_ = 0.0;
};

/// Wraps a source in a genuine slideio::Scene through slideio::makeScene.
std::shared_ptr<slideio::Scene> makeSyntheticScene(const std::shared_ptr<SyntheticSource>& source);

/// Builds the source and the scene in one call, for tests that need no read count.
std::shared_ptr<slideio::Scene> makeSyntheticScene(std::vector<SyntheticLevel> levels,
                                                   std::vector<slideio::DataType> channelTypes,
                                                   PixelFunction pixel);

} // namespace slideio::processor::testing_support
