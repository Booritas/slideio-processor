// Requirements: UR-EXE-1, UR-PIX-6, UR-ALG-5, UR-IMG-4
#pragma once

#include <pipeline/detail/pipeline_impl.hpp>
#include <pipeline/detail/run_plan.hpp>

#include <slideio/processor/core/block_view.hpp>
#include <slideio/processor/core/geometry.hpp>

#include <slideio/slideio/scenemetadata.hpp>
#include <slideio/slideio/scenesource.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace slideio::processor::detail {

/// A pipeline evaluated per requested block: the lazy mode behind
/// Pipeline::levelCount(), rect(), readBlock() and asScene().
///
/// The output pyramid mirrors the source scene's levels from the analysis level
/// down: output level k is the chain evaluated on source level analysis + k, laid
/// out as the run's grid would be at that level, in that level's coordinates.
/// Construction resolves a plan per output level and reads no pixel. Everything
/// is fixed at construction and only read afterwards, so every member may be
/// called from several threads at once. The mask source and the auxiliaries of
/// the snapshot stay borrowed and must outlive the source.
class PipelineSceneSource : public slideio::ISceneSource {
public:
    /// Throws InvalidArgumentError("global-context operations need runInto") when the
    /// chain has a global-context operation, since evaluating a block has no
    /// accumulation pass; otherwise what resolving the plan at each level throws.
    explicit PipelineSceneSource(PipelineSnapshot snapshot);

    /// The number of output levels: the source's levels from the analysis level down.
    int32_t levelCount() const noexcept;

    /// The grid region of output level `level`, in its pixels. Throws
    /// InvalidArgumentError when the level is out of range.
    Rect64 rect(int32_t level) const;

    /// Evaluates `region` of output level `level` into `out`, which must have the
    /// chain's final plane count and types and the region's size. Pixels outside
    /// rect(level) or outside the restriction are zero. Each tile reserves the
    /// level plan's tile bytes from the budget while it is evaluated, waiting until
    /// they fit, except on a thread already holding a tile reservation (see
    /// TileReservationMark), which never waits: it evaluates unreserved when they do
    /// not fit. Throws
    /// InvalidArgumentError for a bad level, region or view; TileError, as a run
    /// reports it, when a tile fails; CancelledError when an operation throws it.
    void readBlock(int32_t level, const Rect64& region, BlockView& out) const;

    /// What makeScene needs to present the output as a scene. Reads the source
    /// scene's metadata, never its pixels. Throws IoError when SlideIO fails.
    slideio::SceneMetadata metadata() const;

    // slideio::ISceneSource. Scene reads resample to the block size nearest-neighbour;
    // level reads name an output level. Throw InvalidArgumentError for a z slice or
    // time frame other than 0, a channel out of range, channels of different types,
    // a non-positive size or a buffer of the wrong size, and what readBlock() above
    // throws.
    void readBlock(const slideio::Rect& sceneRect, const slideio::Size& blockSize,
                   const std::vector<int>& channelIndices, int zSlice, int tFrame, void* buffer,
                   size_t bufferSize) override;
    bool readLevelBlock(int level, const slideio::Rect& levelRect, const slideio::Size& blockSize,
                        const std::vector<int>& channelIndices, int zSlice, int tFrame, void* buffer,
                        size_t bufferSize) override;
    bool supportsConcurrentReads() const override;

private:
    struct Level {
        std::shared_ptr<const RunPlan> plan;
        Rect64 region;  ///< The plan's region, in the level's pixels.
    };

    const Level& levelAt(int32_t level, const char* where) const;

    // Evaluates `levelRect` of output level `level` and delivers `channels`
    // interleaved into `buffer`, resampled nearest-neighbour to `blockSize`.
    void fill(int32_t level, const Rect64& levelRect, const slideio::Size& blockSize,
              const std::vector<int>& channels, int zSlice, int tFrame, void* buffer, size_t bufferSize) const;

    PipelineSnapshot snapshot_;
    std::vector<Level> levels_;
};

} // namespace slideio::processor::detail
