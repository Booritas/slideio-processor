// Requirements: UR-RES-7
#include <slideio/processor/masks/label_scene.hpp>
#include "core/detail/diagnostics.hpp"

#include <slideio/processor/core/block_view.hpp>
#include <slideio/processor/core/data_type.hpp>
#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/flow.hpp>
#include <slideio/processor/reporting/label_mask.hpp>

#include <slideio/core/levelinfo.hpp>
#include <slideio/slideio/scene.hpp>
#include <slideio/slideio/scenemetadata.hpp>
#include <slideio/slideio/scenesource.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <new>
#include <utility>
#include <vector>

namespace slideio::processor {

namespace {

constexpr int32_t kTileSize = 512;

[[noreturn]] void invalid(const char* message) { throw InvalidArgumentError(message); }

// The label mask of one query over one region, as a scene source. Every read builds a
// makeLabelMaskSource() of its own, which only wraps the store, so reads may overlap; the
// store is ThreadSafe. Nothing touches the store until a read.
class LabelSceneSource final : public slideio::ISceneSource {
public:
    LabelSceneSource(Store& store, const ObjectQuery& query, const Rect64& region)
        : store_(&store), query_(query), region_(region) {}

    void readBlock(const slideio::Rect& sceneRect, const slideio::Size& blockSize,
                   const std::vector<int>& channelIndices, int zSlice, int tFrame, void* buffer,
                   size_t bufferSize) override {
        if (zSlice != 0 || tFrame != 0) invalid("label scene: the scene has one z slice and one time frame");
        if (channelIndices.empty()) invalid("label scene: no channel is requested");
        for (int channel : channelIndices) {
            if (channel != 0) invalid("label scene: the scene has one channel");
        }
        if (sceneRect.width <= 0 || sceneRect.height <= 0 || blockSize.width <= 0 || blockSize.height <= 0) {
            invalid("label scene: the rectangle and the block size must be positive");
        }
        const int64_t width = blockSize.width;
        const int64_t height = blockSize.height;
        const size_t pixels = static_cast<size_t>(width) * static_cast<size_t>(height);
        const size_t needed = pixels * channelIndices.size() * sizeof(int32_t);
        if (buffer == nullptr || bufferSize != needed) invalid("label scene: the buffer does not fit the block");

        std::vector<int32_t> plane(pixels, 0);
        BlockPlane view{plane.data(), DataType::Int32, width * static_cast<int64_t>(sizeof(int32_t)), 0};
        const Rect64 slideRect{region_.x + sceneRect.x, region_.y + sceneRect.y, sceneRect.width,
                               sceneRect.height};
        BlockView block(width, height, slideRect, 0, &view, 1);
        TileRasterSourcePtr source = makeLabelMaskSource(*store_, query_);
        source->fillTile(block);
        clearOutside(sceneRect, width, height, plane);

        // One channel, delivered once per requested occurrence, interleaved.
        const size_t copies = channelIndices.size();
        auto* out = static_cast<int32_t*>(buffer);
        if (copies == 1) {
            std::memcpy(out, plane.data(), pixels * sizeof(int32_t));
            return;
        }
        for (size_t i = 0; i < pixels; ++i) {
            for (size_t c = 0; c < copies; ++c) out[i * copies + c] = plane[i];
        }
    }

    bool supportsConcurrentReads() const override { return true; }

private:
    // Zeroes every output pixel whose centre lies outside the scene, (0, 0, region size).
    // SlideIO forwards rectangles unclamped, and objects outside the region are drawn by
    // the label source like any other.
    void clearOutside(const slideio::Rect& rect, int64_t width, int64_t height, std::vector<int32_t>& plane) const {
        // Whether output index i of `count`, spanning `extent` scene pixels from `origin`,
        // has its centre inside [0, limit).
        const auto inside = [](int64_t origin, int64_t extent, int64_t count, int64_t limit) {
            std::vector<char> flags(static_cast<size_t>(count));
            bool all = true;
            for (int64_t i = 0; i < count; ++i) {
                const double centre = static_cast<double>(origin) +
                                      (static_cast<double>(i) + 0.5) * static_cast<double>(extent) /
                                          static_cast<double>(count);
                flags[static_cast<size_t>(i)] = centre >= 0.0 && centre < static_cast<double>(limit);
                all = all && flags[static_cast<size_t>(i)];
            }
            return std::make_pair(all, std::move(flags));
        };
        const auto columns = inside(rect.x, rect.width, width, region_.width);
        const auto rows = inside(rect.y, rect.height, height, region_.height);
        if (columns.first && rows.first) return;
        for (int64_t y = 0; y < height; ++y) {
            int32_t* row = plane.data() + y * width;
            const bool rowInside = rows.second[static_cast<size_t>(y)] != 0;
            for (int64_t x = 0; x < width; ++x) {
                if (!rowInside || columns.second[static_cast<size_t>(x)] == 0) row[x] = 0;
            }
        }
    }

    Store* store_;  // borrowed: the caller keeps it alive for the scene's life
    ObjectQuery query_;
    Rect64 region_;
};

} // namespace

std::shared_ptr<slideio::Scene> rasteriseLabels(Store& store, const ObjectQuery& query, const Rect64& regionLevel0) {
    constexpr int64_t kMaxSide = std::numeric_limits<int32_t>::max();
    if (!query.hasLayer() || !query.layerValue().isValid()) invalid("rasteriseLabels: the query needs a layer");
    const Rect64& r = regionLevel0;
    if (r.width <= 0 || r.height <= 0) invalid("rasteriseLabels: the region is empty");
    if (r.width > kMaxSide || r.height > kMaxSide) invalid("rasteriseLabels: the region is too large for a scene");
    if (r.x > std::numeric_limits<int64_t>::max() - r.width || r.y > std::numeric_limits<int64_t>::max() - r.height) {
        invalid("rasteriseLabels: the region's far edge does not fit");
    }

    const auto width = static_cast<int32_t>(r.width);
    const auto height = static_cast<int32_t>(r.height);
    slideio::SceneMetadata metadata;
    metadata.rect = slideio::Rect(0, 0, width, height);
    metadata.channelDataTypes.push_back(static_cast<slideio::DataType>(dataTypeToSlideio(DataType::Int32)));
    metadata.channelNames.emplace_back("labels");
    // One level: a resampled read reaches the source with its block size and is rasterised
    // at that size, so a pyramid would only add a second, coarser path to the same pixels.
    metadata.levels.emplace_back(0, slideio::Size(width, height), 1.0, 0.0,
                                 slideio::Size(std::min(width, kTileSize), std::min(height, kTileSize)));
    auto source = std::make_shared<LabelSceneSource>(store, query, r);
    try {
        return slideio::makeScene(std::move(source), metadata);
    } catch (const ProcessorError&) {
        throw;
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::exception& e) {
        ErrorContext context;
        const std::string causeText = detail::diagnosticText(e);
        context.cause = causeText.c_str();
        throw InternalError("rasteriseLabels: SlideIO refused the scene's description", context);
    }
}

} // namespace slideio::processor
