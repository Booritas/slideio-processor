// Requirements: UR-IMG-9, UR-IMG-4, UR-EXT-3
#pragma once

#include <pipeline/detail/level_table.hpp>
#include <pipeline/detail/plane_buffers.hpp>

#include <slideio/processor/core/data_type.hpp>
#include <slideio/processor/core/geometry.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace slideio {
class Scene;
} // namespace slideio

namespace slideio::processor::detail {

/// Reads planar blocks of one pyramid level from a SlideIO scene.
/// Spelled with std::shared_ptr<slideio::Scene> because SlideIO defines ScenePtr
/// as a macro. Concurrent reads go through the scene's own thread-safety.
class SceneReader {
public:
    /// Throws InvalidArgumentError for a null scene or a level of non-positive
    /// size; IoError, with the cause, when SlideIO cannot describe the scene or
    /// reports no level where it counted one.
    explicit SceneReader(std::shared_ptr<slideio::Scene> scene);

    /// The type of every scene channel; DataType::Unknown for a channel whose
    /// SlideIO type has no pipeline equivalent (read() rejects such a channel).
    std::vector<DataType> channelTypes() const;

    /// The scene's pyramid, read once at construction, as levelTable() builds it.
    const std::vector<LevelDims>& levels() const noexcept;

    /// The path of the file the scene reports, "" when it reports none; named in the
    /// messages of SlideIO failures.
    const std::string& slidePath() const noexcept;

    /// Fills `out` with `channels` of `levelRect` at `level`. `out` must have
    /// levelRect's size, one plane per channel, each plane holding that channel
    /// with its type. The part of the rect outside the level is zero-filled here,
    /// never by SlideIO. Throws InvalidArgumentError for a bad level, channel,
    /// rect or buffer, UnsupportedDataTypeError for an Unknown channel type, and
    /// IoError, with the cause, when SlideIO fails.
    void read(int32_t level, const Rect64& levelRect, const std::vector<int32_t>& channels,
              PlaneBuffers& out) const;

private:
    std::shared_ptr<slideio::Scene> scene_;
    std::string slidePath_;                        // "" when the scene reports no file
    std::vector<DataType> types_;                  // Unknown kept; read() rejects it for a selected channel
    std::vector<LevelDims> levels_;
};

} // namespace slideio::processor::detail
