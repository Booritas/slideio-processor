// Requirements: UR-EXT-3, UR-IMG-5, UR-ALG-5, UR-EXE-1, UR-STG-3
#include <pipeline/detail/tile_evaluator.hpp>

#include <pipeline/detail/level_mapping.hpp>
#include <pipeline/detail/operation_context_state.hpp>
#include <pipeline/detail/plane_buffers.hpp>

#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/object_record.hpp>

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>

namespace slideio::processor::detail {

namespace {

Rect64 grow(const Rect64& r, const Halo& h) {
    return Rect64{r.x - h.left, r.y - h.top, r.width + h.left + h.right, r.height + h.top + h.bottom};
}

bool holds(const Rect64& r, Point64 p) {
    return p.x >= r.x && p.y >= r.y && p.x - r.x < r.width && p.y - r.y < r.height;
}

// The object sink an operation sees on one tile: an object is kept only when its
// anchor lies in the tile's level-0 rect (P20), then only when the tile's mask is
// set there (P24). Flushing is the run's business, so flush() does nothing.
class TileObjectFilter : public ObjectSink {
public:
    TileObjectFilter(ObjectSink* target, const Rect64& tileRect, const RasterMask* mask, RunCounters& counters)
        : target_(target), tileRect_(tileRect), mask_(mask), counters_(counters) {}

    void emit(const ObjectRecord& record) override {
        const Point64 anchor = record.geometry().anchor();
        if (!holds(tileRect_, anchor)) {
            ++counters_.droppedOutsideTile;
            return;
        }
        if (mask_ != nullptr && !mask_->testLevel0(anchor)) {
            ++counters_.droppedByRestriction;
            return;
        }
        target_->emit(record);
        ++emitted_;
    }
    void flush() override {}
    int64_t emittedCount() const override { return emitted_; }

private:
    ObjectSink* target_;
    Rect64 tileRect_;
    const RasterMask* mask_;
    RunCounters& counters_;
    int64_t emitted_ = 0;
};

// Plane `plane` of `view` as a view of its own.
BlockView singlePlane(const BlockView& view, int32_t plane) {
    const BlockPlane p{view.planeData(plane), view.planeType(plane), view.planeRowStrideBytes(plane),
                       view.planeSourceChannel(plane)};
    return BlockView(view.width(), view.height(), view.slideRect(), view.level(), &p, 1);
}

// Zeroes every sample of `buffers`, which cover `rect`, lying outside `extent`, so
// a later operation reads beyond the level exactly as it reads the source there.
void zeroOutside(PlaneBuffers& buffers, const Rect64& rect, const Rect64& extent) {
    const Rect64 keep = rect.intersected(extent);
    for (int32_t plane = 0; plane < buffers.planeCount(); ++plane) {
        const int64_t size = dataTypeSizeBytes(buffers.planeType(plane));
        const int64_t stride = buffers.rowStrideBytes(plane);
        uint8_t* data = buffers.planeData(plane);
        for (int64_t y = 0; y < rect.height; ++y) {
            uint8_t* row = data + y * stride;
            const int64_t levelY = rect.y + y;
            if (keep.isEmpty() || levelY < keep.y || levelY >= keep.y + keep.height) {
                std::memset(row, 0, static_cast<size_t>(stride));
                continue;
            }
            const int64_t from = keep.x - rect.x;
            const int64_t to = from + keep.width;
            std::memset(row, 0, static_cast<size_t>(from * size));
            std::memset(row + to * size, 0, static_cast<size_t>((rect.width - to) * size));
        }
    }
}

// The scene channel each output plane of operation `op` reports: a PerChannel
// plane keeps its input plane's; plane j of a Joint output reports input plane j's,
// or the last input plane's when there are fewer.
std::vector<int32_t> outputChannels(const PlaneBuffers& input, size_t outputCount) {
    std::vector<int32_t> channels;
    for (size_t j = 0; j < outputCount; ++j) {
        const int32_t from = static_cast<int32_t>(std::min(j, static_cast<size_t>(input.planeCount() - 1)));
        channels.push_back(input.sourceChannel(from));
    }
    return channels;
}

template <typename T>
T& required(T* binding, const char* what) {
    if (binding == nullptr) {
        throw InvalidArgumentError((std::string("TileEvaluator: the ") + what + " is null").c_str());
    }
    return *binding;
}

} // namespace

TileEvaluator::TileEvaluator(const RunPlan& plan, const SceneReader& reader, Bindings bindings)
    : plan_(plan),
      reader_(reader),
      bindings_(std::move(bindings)),
      restriction_(plan, required(bindings_.spec, "spec"), required(bindings_.selector, "level selector"),
                   bindings_.maskSource) {
    required(bindings_.counters, "run counters");
}

const RestrictionFilter& TileEvaluator::restriction() const noexcept { return restriction_; }

TileOutcome TileEvaluator::evaluate(int64_t tileIndex, const CancellationToken& token) const {
    token.throwIfCancelled();
    const TileGrid& grid = plan_.grid;
    const Rect64 tile = grid.tileRectAtLevel(tileIndex);
    int64_t bytes = 0;

    // The mask comes first: an excluded tile reads no pixel (P24).
    const RasterMask mask = restriction_.buildTileMask(tileIndex);
    if (!mask.isNull()) {
        bytes += mask.regionAtLevel().width * mask.regionAtLevel().height;
    }
    if (restriction_.skips(mask, tileIndex)) {
        ++bindings_.counters->tilesSkipped;
        return TileOutcome{true, bytes};
    }
    const RasterMask* tileMask = mask.isNull() ? nullptr : &mask;

    // Operation k writes the tile grown by the halos of the operations after it (P25).
    const std::vector<OperationPtr>& operations = plan_.operations;
    std::vector<Rect64> outputRects(operations.size());
    Rect64 sourceRect = tile;
    for (size_t k = operations.size(); k-- > 0;) {
        outputRects[k] = sourceRect;
        sourceRect = grow(sourceRect, operations[k]->halo());
    }

    PlaneBuffers current(sourceRect.width, sourceRect.height, plan_.planeTypes[0], plan_.channels);
    bytes += current.bytes();
    reader_.read(plan_.level, sourceRect, plan_.channels, current);
    Rect64 currentRect = sourceRect;

    TileObjectFilter objects(bindings_.objectSink, grid.tileRect(tileIndex), tileMask, *bindings_.counters);

    for (size_t k = 0; k < operations.size(); ++k) {
        token.throwIfCancelled();
        Operation& op = *operations[k];
        const Rect64& outputRect = outputRects[k];
        const std::vector<DataType>& outputTypes = plan_.planeTypes[k + 1];
        PlaneBuffers output(outputRect.width, outputRect.height, outputTypes,
                            outputChannels(current, outputTypes.size()));
        bytes += output.bytes();
        const BlockView inputView = current.view(plan_.level, currentRect, plan_.levelScale);
        const BlockView outputView = output.view(plan_.level, outputRect, plan_.levelScale);

        // Auxiliary rasters cover the operation's input rect.
        std::vector<PlaneBuffers> auxiliaryBuffers;
        std::vector<BlockView> auxiliaryViews;
        auxiliaryBuffers.reserve(bindings_.auxiliaries.size());
        auxiliaryViews.reserve(bindings_.auxiliaries.size());
        for (const auto& auxiliary : bindings_.auxiliaries) {
            const int32_t count = auxiliary.second->planeCount();
            std::vector<int32_t> channels;
            for (int32_t c = 0; c < std::max<int32_t>(count, 0); ++c) {
                channels.push_back(c);
            }
            auxiliaryBuffers.emplace_back(currentRect.width, currentRect.height,
                                          std::vector<DataType>(channels.size(), auxiliary.second->planeType()),
                                          channels);
            bytes += auxiliaryBuffers.back().bytes();
            auxiliaryViews.push_back(auxiliaryBuffers.back().view(plan_.level, currentRect, plan_.levelScale));
            auxiliary.second->fillTile(auxiliaryViews.back());
        }

        OperationContextState state;
        state.objectSink = op.producesObjects() && bindings_.objectSink != nullptr ? &objects : nullptr;
        state.edgeSink = bindings_.edgeSink;
        state.annotationSink = bindings_.annotationSink;
        for (size_t a = 0; a < bindings_.auxiliaries.size(); ++a) {
            state.auxiliaries.emplace_back(bindings_.auxiliaries[a].first, &auxiliaryViews[a]);
        }
        state.targetLayer = plan_.targetLayers[k];
        state.level = plan_.level;
        state.tileRectAtLevel = outputRect;
        state.haloRectAtLevel = currentRect;
        state.tileRect = rectToLevel0(outputRect, plan_.levelScale);
        state.haloRect = rectToLevel0(currentRect, plan_.levelScale);
        state.levelScale = plan_.levelScale;
        state.channels = &bindings_.spec->channels();
        state.cancellation = &token;
        state.mask = tileMask;
        state.seamFragment = bindings_.seamFragment;

        if (op.arity() == ChannelArity::PerChannel) {
            for (int32_t plane = 0; plane < inputView.planeCount(); ++plane) {
                const BlockView in = singlePlane(inputView, plane);
                BlockView out = singlePlane(outputView, plane);
                OperationContextState planeState = state;
                planeState.input = &in;
                planeState.output = &out;
                op.apply(*makeOperationContext(std::move(planeState)));
            }
        } else {
            BlockView out = outputView;
            state.input = &inputView;
            state.output = &out;
            op.apply(*makeOperationContext(std::move(state)));
        }

        if (k + 1 < operations.size()) {
            zeroOutside(output, outputRect, plan_.levelRect);
        }
        current = std::move(output);
        currentRect = outputRect;
    }

    if (bindings_.rasterSink != nullptr) {
        const BlockView result = current.view(plan_.level, tile, plan_.levelScale);
        bindings_.rasterSink->writeTile(result);
    }
    return TileOutcome{false, bytes};
}

} // namespace slideio::processor::detail
