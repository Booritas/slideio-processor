// Requirements: UR-EXE-1, UR-PIX-6, UR-ALG-5, UR-IMG-4
#include <pipeline/detail/pipeline_scene_source.hpp>

#include <pipeline/detail/plane_buffers.hpp>
#include <pipeline/detail/pixel_size.hpp>
#include <pipeline/detail/tile_evaluator.hpp>
#include <pipeline/detail/tile_reservation.hpp>

#include "core/detail/diagnostics.hpp"
#include <slideio/processor/core/control.hpp>
#include <slideio/processor/core/data_type.hpp>
#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/object_record.hpp>
#include <slideio/processor/core/raster_mask.hpp>

#include <slideio/core/levelinfo.hpp>
#include <slideio/slideio/scene.hpp>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <exception>
#include <new>
#include <numeric>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace slideio::processor::detail {

namespace {

[[noreturn]] void invalid(const char* where, const std::string& what) {
    throw InvalidArgumentError((std::string(where) + ": " + what).c_str());
}

bool overlaps(int64_t a, int64_t aLength, int64_t b, int64_t bLength) {
    return a < b + bLength && b < a + aLength;
}

// Holds bytes reserved from a budget and releases them on scope exit.
class Reservation {
public:
    Reservation(MemoryBudget& budget, int64_t bytes) : budget_(budget), bytes_(bytes) {}
    ~Reservation() {
        try {
            budget_.release(bytes_);
        } catch (...) {
            // release() refuses only bytes it never reserved, which this scope cannot cause.
        }
    }
    Reservation(const Reservation&) = delete;
    Reservation& operator=(const Reservation&) = delete;

private:
    MemoryBudget& budget_;
    int64_t bytes_;
};

// Objects emitted while evaluating a block are not part of a block: dropped.
class DiscardingObjectSink : public ObjectSink {
public:
    void emit(const ObjectRecord&) override {}
    void flush() override {}
    int64_t emittedCount() const override { return 0; }
};

// Receives the final output of each tile of one readBlock call, on the calling
// thread, and copies the part lying over the requested region into the caller's
// view, zeroing what the restriction excludes. The tile in progress, and the rect
// its written view covers, are set before each evaluation.
class BlockCopySink : public RasterSink {
public:
    BlockCopySink(BlockView& out, const Rect64& region) : out_(out), region_(region) {}

    void setTile(int64_t index, const Rect64& rectAtLevel) {
        tileIndex_ = index;
        tileRect_ = rectAtLevel;
    }
    void setRestriction(const RestrictionFilter* restriction) { restriction_ = restriction; }

    void writeTile(const BlockView& tile) override {
        const Rect64 part = tileRect_.intersected(region_);
        if (part.isEmpty()) {
            return;
        }
        for (int32_t plane = 0; plane < out_.planeCount(); ++plane) {
            const int64_t size = dataTypeSizeBytes(out_.planeType(plane));
            const auto* from = static_cast<const uint8_t*>(tile.planeData(plane));
            auto* to = static_cast<uint8_t*>(out_.planeData(plane));
            for (int64_t y = part.y; y < part.y + part.height; ++y) {
                std::memcpy(to + (y - region_.y) * out_.planeRowStrideBytes(plane) + (part.x - region_.x) * size,
                            from + (y - tileRect_.y) * tile.planeRowStrideBytes(plane) + (part.x - tileRect_.x) * size,
                            static_cast<size_t>(part.width * size));
            }
        }
        ++written_;
        if (restriction_ == nullptr || !restriction_->restricted()) {
            return;
        }
        const RasterMask mask = restriction_->buildTileMask(tileIndex_);
        for (int64_t y = part.y; y < part.y + part.height; ++y) {
            for (int64_t x = part.x; x < part.x + part.width; ++x) {
                if (mask.test(Point64{x, y})) {
                    continue;
                }
                for (int32_t plane = 0; plane < out_.planeCount(); ++plane) {
                    const int64_t size = dataTypeSizeBytes(out_.planeType(plane));
                    std::memset(static_cast<uint8_t*>(out_.planeData(plane)) +
                                    (y - region_.y) * out_.planeRowStrideBytes(plane) + (x - region_.x) * size,
                                0, static_cast<size_t>(size));
                }
            }
        }
    }
    void flush() override {}
    int64_t tilesWritten() const override { return written_; }

private:
    BlockView& out_;
    Rect64 region_;
    const RestrictionFilter* restriction_ = nullptr;
    int64_t tileIndex_ = -1;
    Rect64 tileRect_{};
    int64_t written_ = 0;
};

// Rethrows the exception in flight as a run reports a tile failure: a
// CancelledError as is, anything else as a TileError naming the stage, the
// operation when one threw, and the tile.
[[noreturn]] void reportTileFailure(const std::string& stageName, const char* operationName, const TileGrid& grid,
                                    int64_t tile) {
    std::string cause;
    try {
        throw;
    } catch (const CancelledError&) {
        throw;
    } catch (const std::exception& e) {
        cause = detail::diagnosticText(e);
    } catch (...) {
        cause = "an exception that is not a std::exception";
    }
    ErrorContext context;
    context.stageName = stageName.c_str();
    context.operationName = operationName;
    context.cause = cause.c_str();
    context.hasTile = true;
    context.tileX = tile % grid.columns();
    context.tileY = tile / grid.columns();
    throw TileError("a tile failed", context);
}

Rect64 toRect64(const slideio::Rect& r) { return Rect64{r.x, r.y, r.width, r.height}; }

} // namespace

PipelineSceneSource::PipelineSceneSource(PipelineSnapshot snapshot) : snapshot_(std::move(snapshot)) {
    if (!snapshot_.plan || !snapshot_.reader || !snapshot_.selector || !snapshot_.budget || !snapshot_.scene) {
        throw InternalError("PipelineSceneSource: the snapshot is incomplete");
    }
    if (!snapshot_.plan->globalContextPositions.empty()) {
        throw InvalidArgumentError("global-context operations need runInto");
    }
    const int32_t analysis = snapshot_.plan->level;
    const int32_t sourceLevels = static_cast<int32_t>(snapshot_.levels.size());
    const std::vector<DataType> channelTypes = snapshot_.reader->channelTypes();
    for (int32_t level = analysis; level < sourceLevels; ++level) {
        std::shared_ptr<const RunPlan> plan = snapshot_.plan;
        if (level != analysis) {
            // The same chain, laid out at the coarser level as validate() would lay it out there.
            PipelineSpec spec = snapshot_.spec;
            spec.analysisLevel(level);
            const RunPlanInputs inputs{&spec,
                                       &snapshot_.config,
                                       &channelTypes,
                                       snapshot_.selector.get(),
                                       &snapshot_.levels,
                                       &snapshot_.layers,
                                       &snapshot_.auxiliaries,
                                       snapshot_.maskSource != nullptr,
                                       snapshot_.budget->ceilingBytes()};
            plan = std::make_shared<const RunPlan>(buildRunPlan(inputs));
        }
        levels_.push_back(Level{plan, plan->region});
    }
}

int32_t PipelineSceneSource::levelCount() const noexcept { return static_cast<int32_t>(levels_.size()); }

const PipelineSceneSource::Level& PipelineSceneSource::levelAt(int32_t level, const char* where) const {
    if (level < 0 || level >= levelCount()) {
        invalid(where, "level " + std::to_string(level) + " is out of range [0, " + std::to_string(levelCount()) +
                           ")");
    }
    return levels_[static_cast<size_t>(level)];
}

Rect64 PipelineSceneSource::rect(int32_t level) const { return levelAt(level, "Pipeline::rect").region; }

void PipelineSceneSource::readBlock(int32_t level, const Rect64& region, BlockView& out) const {
    constexpr const char* where = "Pipeline::readBlock";
    const Level& entry = levelAt(level, where);
    const RunPlan& plan = *entry.plan;
    const std::vector<DataType>& types = plan.planeTypes.back();
    if (region.width <= 0 || region.height <= 0) {
        invalid(where, "the region is empty");
    }
    if (out.isNull()) {
        invalid(where, "the output view is null");
    }
    if (out.width() != region.width || out.height() != region.height) {
        invalid(where, "the output view is " + std::to_string(out.width()) + " x " + std::to_string(out.height()) +
                           ", the region " + std::to_string(region.width) + " x " + std::to_string(region.height));
    }
    if (out.planeCount() != static_cast<int32_t>(types.size())) {
        invalid(where, "the output view has " + std::to_string(out.planeCount()) + " planes, the chain yields " +
                           std::to_string(types.size()));
    }
    for (int32_t plane = 0; plane < out.planeCount(); ++plane) {
        const DataType expected = types[static_cast<size_t>(plane)];
        if (out.planeType(plane) != expected) {
            invalid(where, "plane " + std::to_string(plane) + " of the output view is " +
                               dataTypeName(out.planeType(plane)) + ", the chain yields " + dataTypeName(expected));
        }
        if (out.planeData(plane) == nullptr ||
            out.planeRowStrideBytes(plane) < out.width() * dataTypeSizeBytes(expected)) {
            invalid(where, "plane " + std::to_string(plane) + " of the output view has no room for a row");
        }
    }

    for (int32_t plane = 0; plane < out.planeCount(); ++plane) {
        const size_t rowBytes = static_cast<size_t>(out.width() * dataTypeSizeBytes(out.planeType(plane)));
        auto* data = static_cast<uint8_t*>(out.planeData(plane));
        for (int64_t y = 0; y < out.height(); ++y) {
            std::memset(data + y * out.planeRowStrideBytes(plane), 0, rowBytes);
        }
    }
    const Rect64 within = region.intersected(entry.region);
    if (within.isEmpty()) {
        return;
    }

    // The grid's columns and rows that meet the region.
    const TileGrid& grid = plan.grid;
    std::vector<int64_t> columns;
    for (int64_t column = 0; column < grid.columns(); ++column) {
        const Rect64 tile = grid.tileRectAtLevel(grid.tileIndex(column, 0));
        if (overlaps(tile.x, tile.width, within.x, within.width)) {
            columns.push_back(column);
        }
    }
    std::vector<int64_t> rows;
    for (int64_t row = 0; row < grid.rows(); ++row) {
        const Rect64 tile = grid.tileRectAtLevel(grid.tileIndex(0, row));
        if (overlaps(tile.y, tile.height, within.y, within.height)) {
            rows.push_back(row);
        }
    }

    RunCounters counters;
    DiscardingObjectSink discard;
    BlockCopySink copy(out, region);
    Bindings bindings;
    bindings.objectSink = &discard;
    bindings.rasterSink = &copy;
    bindings.auxiliaries = snapshot_.auxiliaries;
    bindings.maskSource = snapshot_.maskSource;
    bindings.seamFragment = [](const Polygon&, LayerId, const IntensityPartial*, int32_t, int64_t, int64_t) {};
    bindings.spec = &snapshot_.spec;
    bindings.selector = snapshot_.selector.get();
    bindings.counters = &counters;
    bindings.pixelSizeMicrons = levelZeroPixelSizeMicrons(*snapshot_.scene);
    const TileEvaluator evaluator(plan, plan.operations, *snapshot_.reader, std::move(bindings));
    copy.setRestriction(&evaluator.restriction());

    MemoryBudget& budget = *snapshot_.budget;
    const CancellationToken never;
    for (int64_t row : rows) {
        for (int64_t column : columns) {
            const int64_t tile = grid.tileIndex(column, row);
            // The evaluator writes the part of the tile inside the plan's region.
            copy.setTile(tile, grid.tileRectAtLevel(tile).intersected(entry.region));
            // As in a run: the plan's tile bytes bound the tile's peak buffers. A thread
            // already holding a tile reservation (a pipeline tile reading this scene) never
            // waits: its bytes, and its fellow workers', are freed only by tiles, so waiting
            // could deadlock. It reserves if the bytes fit and evaluates unreserved if not.
            int64_t reserved = 0;
            if (!TileReservationMark::held()) {
                budget.reserveOrWait(plan.tileBytes, never);
                reserved = plan.tileBytes;
            } else if (budget.tryReserve(plan.tileBytes)) {
                reserved = plan.tileBytes;
            }
            const Reservation held(budget, reserved);
            const TileReservationMark mark;
            const char* operation = nullptr;
            try {
                evaluator.evaluate(tile, never, &operation);
            } catch (...) {
                reportTileFailure(snapshot_.stageName, operation, grid, tile);
            }
        }
    }
}

slideio::SceneMetadata PipelineSceneSource::metadata() const {
    const int32_t analysis = snapshot_.plan->level;
    const std::vector<DataType>& types = levels_.front().plan->planeTypes.back();
    slideio::SceneMetadata m;
    for (size_t channel = 0; channel < types.size(); ++channel) {
        m.channelDataTypes.push_back(static_cast<slideio::DataType>(dataTypeToSlideio(types[channel])));
        m.channelNames.push_back("c" + std::to_string(channel));
    }
    try {
        const slideio::Scene& scene = *snapshot_.scene;
        m.name = scene.getName() + "/pipeline";
        // Output level 0 is the analysis level: the source's own description when that
        // is level 0, otherwise rescaled to it, since a scene's level 0 has scale 1.
        const LevelDims& top = snapshot_.levels[static_cast<size_t>(analysis)];
        const std::tuple<int, int, int, int> rect = scene.getRect();
        m.rect = slideio::Rect(std::get<0>(rect), std::get<1>(rect), static_cast<int32_t>(top.width),
                               static_cast<int32_t>(top.height));
        const double factor = snapshot_.selector->scaleOfLevel(analysis);
        const std::tuple<double, double> resolution = scene.getResolution();
        m.resolution = slideio::Resolution(std::get<0>(resolution) * factor, std::get<1>(resolution) * factor);
        m.magnification = scene.getMagnification() / factor;
        m.colorProfile = scene.getColorProfile();

        // A scene reporting no level has one, the scene rect, at scale 1.
        const int32_t reported = scene.getNumZoomLevels();
        const double baseScale = analysis < reported ? scene.getLevelInfo(analysis)->getScale() : 1.0;
        for (int32_t k = 0; k < levelCount(); ++k) {
            const int32_t sourceLevel = analysis + k;
            const LevelDims& dims = snapshot_.levels[static_cast<size_t>(sourceLevel)];
            double scale = 1.0;
            double magnification = m.magnification;
            if (sourceLevel < reported) {
                const slideio::LevelInfo* info = scene.getLevelInfo(sourceLevel);
                scale = info->getScale() / baseScale;
                magnification = info->getMagnification();
            }
            int32_t tileWidth = 0;
            int32_t tileHeight = 0;
            defaultTileSize(snapshot_.scene, sourceLevel, snapshot_.config, tileWidth, tileHeight);
            m.levels.emplace_back(k, slideio::Size(static_cast<int32_t>(dims.width), static_cast<int32_t>(dims.height)),
                                  scale, magnification, slideio::Size(tileWidth, tileHeight));
        }
    } catch (const ProcessorError&) {
        throw;
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::exception& e) {
        ErrorContext context;
        const std::string causeText = detail::diagnosticText(e);
        context.cause = causeText.c_str();
        throw IoError("PipelineSceneSource: SlideIO failed to describe the source scene", context);
    }
    return m;
}

void PipelineSceneSource::readBlock(const slideio::Rect& sceneRect, const slideio::Size& blockSize,
                                    const std::vector<int>& channelIndices, int zSlice, int tFrame, void* buffer,
                                    size_t bufferSize) {
    constexpr const char* where = "PipelineSceneSource::readBlock";
    if (sceneRect.width <= 0 || sceneRect.height <= 0 || blockSize.width <= 0 || blockSize.height <= 0) {
        invalid(where, "the rectangle and the block size must be positive");
    }
    // The coarsest output level whose resolution is at least the one requested on
    // both axes. Output level k is source level analysis + k, so the fraction is
    // taken relative to source level 0 for the selector.
    const double fraction = std::max(static_cast<double>(blockSize.width) / sceneRect.width,
                                     static_cast<double>(blockSize.height) / sceneRect.height);
    const int32_t analysis = snapshot_.plan->level;
    const LevelSelector& selector = *snapshot_.selector;
    int32_t level = 0;
    if (fraction < 1.0) {
        const int32_t sourceLevel = selector.levelForScale(fraction / selector.scaleOfLevel(analysis));
        level = std::min(std::max(sourceLevel - analysis, 0), levelCount() - 1);
    }
    const Rect64 rect = toRect64(sceneRect);
    const Rect64 levelRect =
        level == 0 ? rect : selector.mapToLevel(selector.mapToLevel0(rect, analysis), analysis + level);
    fill(level, levelRect, blockSize, channelIndices, zSlice, tFrame, buffer, bufferSize);
}

bool PipelineSceneSource::readLevelBlock(int level, const slideio::Rect& levelRect, const slideio::Size& blockSize,
                                         const std::vector<int>& channelIndices, int zSlice, int tFrame,
                                         void* buffer, size_t bufferSize) {
    fill(level, toRect64(levelRect), blockSize, channelIndices, zSlice, tFrame, buffer, bufferSize);
    return true;
}

bool PipelineSceneSource::supportsConcurrentReads() const { return true; }

void PipelineSceneSource::fill(int32_t level, const Rect64& levelRect, const slideio::Size& blockSize,
                               const std::vector<int>& channels, int zSlice, int tFrame, void* buffer,
                               size_t bufferSize) const {
    constexpr const char* where = "PipelineSceneSource::readLevelBlock";
    if (zSlice != 0 || tFrame != 0) {
        invalid(where, "the scene has one z slice and one time frame");
    }
    const Level& entry = levelAt(level, where);
    const std::vector<DataType>& types = entry.plan->planeTypes.back();
    if (channels.empty()) {
        invalid(where, "no channel is requested");
    }
    for (int channel : channels) {
        if (channel < 0 || channel >= static_cast<int>(types.size())) {
            invalid(where, "channel " + std::to_string(channel) + " is out of range [0, " +
                               std::to_string(types.size()) + ")");
        }
    }
    const DataType type = types[static_cast<size_t>(channels.front())];
    for (int channel : channels) {
        if (types[static_cast<size_t>(channel)] != type) {
            invalid(where, "the requested channels differ in type; read them one type per call");
        }
    }
    if (blockSize.width <= 0 || blockSize.height <= 0 || levelRect.width <= 0 || levelRect.height <= 0) {
        invalid(where, "the rectangle and the block size must be positive");
    }
    const int64_t sampleBytes = dataTypeSizeBytes(type);
    const int64_t pixelBytes = static_cast<int64_t>(channels.size()) * sampleBytes;
    const int64_t needed = static_cast<int64_t>(blockSize.width) * blockSize.height * pixelBytes;
    if (buffer == nullptr || static_cast<int64_t>(bufferSize) != needed) {
        invalid(where, "the buffer holds " + std::to_string(bufferSize) + " bytes, the block needs " +
                           std::to_string(needed));
    }

    // Planar, as the chain computes, then interleaved into the caller's buffer.
    std::vector<int32_t> planeChannels(types.size());
    std::iota(planeChannels.begin(), planeChannels.end(), 0);
    PlaneBuffers planes(levelRect.width, levelRect.height, types, planeChannels);
    BlockView view = planes.view(entry.plan->level, levelRect, entry.plan->levelScale);
    readBlock(level, levelRect, view);

    // Nearest-neighbour, the mapping of cv::INTER_NEAREST: output sample d takes input
    // sample floor(d * input / output), so no value is invented. Done here, in the
    // interleaving pass, so every sample type is served alike.
    std::vector<int64_t> columns(static_cast<size_t>(blockSize.width));
    for (int64_t x = 0; x < blockSize.width; ++x) {
        columns[static_cast<size_t>(x)] = x * levelRect.width / blockSize.width;
    }
    std::vector<const uint8_t*> rows(channels.size());
    auto* to = static_cast<uint8_t*>(buffer);
    for (int64_t y = 0; y < blockSize.height; ++y) {
        const int64_t from = y * levelRect.height / blockSize.height;
        for (size_t i = 0; i < channels.size(); ++i) {
            rows[i] = planes.planeData(channels[i]) + from * planes.rowStrideBytes(channels[i]);
        }
        for (int64_t x = 0; x < blockSize.width; ++x) {
            const int64_t offset = columns[static_cast<size_t>(x)] * sampleBytes;
            for (size_t i = 0; i < channels.size(); ++i) {
                std::memcpy(to, rows[i] + offset, static_cast<size_t>(sampleBytes));
                to += sampleBytes;
            }
        }
    }
}

} // namespace slideio::processor::detail
