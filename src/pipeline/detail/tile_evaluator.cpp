// Requirements: UR-EXT-3, UR-IMG-5, UR-ALG-5, UR-EXE-1, UR-STG-3, UR-SEG-3
#include <pipeline/detail/tile_evaluator.hpp>

#include <pipeline/detail/finishing_sink.hpp>
#include <pipeline/detail/fragment_merge.hpp>
#include <pipeline/detail/level_mapping.hpp>
#include <pipeline/detail/operation_context_state.hpp>
#include <pipeline/detail/plane_buffers.hpp>

#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/object_record.hpp>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace slideio::processor::detail {

namespace {

Rect64 grow(const Rect64& r, const Halo& h) {
    return Rect64{r.x - h.left, r.y - h.top, r.width + h.left + h.right, r.height + h.top + h.bottom};
}

bool holds(const Rect64& r, Point64 p) {
    return p.x >= r.x && p.y >= r.y && p.x - r.x < r.width && p.y - r.y < r.height;
}

} // namespace

TileObjectFilter::TileObjectFilter(ObjectSink* target, const Rect64& tileRect, const RasterMask* mask,
                                   RunCounters& counters)
    : target_(target), finishing_(dynamic_cast<FinishingSink*>(target)), tileRect_(tileRect), mask_(mask),
      counters_(counters) {}

void TileObjectFilter::emit(const ObjectRecord& record) {
    const Point64 anchor = record.geometry().anchor();
    if (!holds(tileRect_, anchor)) {
        ++counters_.droppedOutsideTile;
        return;
    }
    if (mask_ != nullptr && !mask_->testLevel0(anchor)) {
        ++counters_.droppedByRestriction;
        return;
    }
    if (ringsAreNormalised(record.geometry())) {
        target_->emit(record);
    } else {
        ObjectRecord normalised(record);
        {
            Polygon geometry = record.geometry();
            normaliseRings(geometry);
            normalised.setGeometry(geometry);
        }
        if (finishing_ != nullptr) {
            finishing_->emitOwned(normalised);
        } else {
            target_->emit(normalised);
        }
    }
    ++emitted_;
}

namespace {

// Plane `plane` of `view` as a view of its own.
BlockView singlePlane(const BlockView& view, int32_t plane) {
    const BlockPlane p{view.planeData(plane), view.planeType(plane), view.planeRowStrideBytes(plane),
                       view.planeSourceChannel(plane)};
    return BlockView(view.width(), view.height(), view.slideRect(), view.level(), &p, 1);
}

// The part of `buffers`, which cover `bufferRect`, lying over `rect` (inside it), as a
// view of its own: same memory, offset and stride, and the edge-mapped slideRect of `rect`.
BlockView subView(PlaneBuffers& buffers, const Rect64& bufferRect, const Rect64& rect, int32_t level,
                  double levelScale) {
    std::vector<BlockPlane> planes;
    for (int32_t plane = 0; plane < buffers.planeCount(); ++plane) {
        const int64_t size = dataTypeSizeBytes(buffers.planeType(plane));
        const int64_t stride = buffers.rowStrideBytes(plane);
        uint8_t* first = buffers.planeData(plane) + (rect.y - bufferRect.y) * stride + (rect.x - bufferRect.x) * size;
        planes.push_back(BlockPlane{first, buffers.planeType(plane), stride, buffers.sourceChannel(plane)});
    }
    return BlockView(rect.width, rect.height, rectToLevel0(rect, levelScale), level, planes.data(),
                     static_cast<int32_t>(planes.size()));
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

TileEvaluator::TileEvaluator(const RunPlan& plan, std::vector<OperationPtr> operations, const SceneReader& reader,
                             Bindings bindings)
    : plan_(plan),
      operations_(std::move(operations)),
      reader_(reader),
      bindings_(std::move(bindings)),
      restriction_(plan, required(bindings_.spec, "spec"), required(bindings_.selector, "level selector"),
                   bindings_.maskSource, bindings_.memoryBudget) {
    required(bindings_.counters, "run counters");
    if (operations_.size() != plan_.operations.size() ||
        std::any_of(operations_.begin(), operations_.end(), [](const OperationPtr& op) { return !op; })) {
        throw InvalidArgumentError("TileEvaluator: the operations do not match the plan's chain");
    }
}

const RestrictionFilter& TileEvaluator::restriction() const noexcept { return restriction_; }

TileOutcome TileEvaluator::evaluate(int64_t tileIndex, const CancellationToken& token,
                                    const char** runningOperation, const Accumulation* accumulation) const {
    if (runningOperation != nullptr) {
        *runningOperation = nullptr;
    }
    const std::vector<OperationPtr>& operations = operations_;
    if (accumulation != nullptr &&
        (accumulation->position >= operations.size() || accumulation->accumulator == nullptr)) {
        throw InvalidArgumentError("TileEvaluator::evaluate: the accumulation names no operation");
    }
    token.throwIfCancelled();
    const TileGrid& grid = plan_.grid;
    const Rect64 tile = grid.tileRectAtLevel(tileIndex);
    int64_t bytes = 0;

    // The mask comes first: an excluded tile reads no pixel.
    const RasterMask mask = restriction_.buildTileMask(tileIndex);
    if (!mask.isNull()) {
        bytes += mask.regionAtLevel().width * mask.regionAtLevel().height;
    }
    if (restriction_.skips(mask, tileIndex)) {
        if (accumulation == nullptr) {
            ++bindings_.counters->tilesSkipped;  // Once per run: the final pass counts it.
        }
        return TileOutcome{true, bytes};
    }
    const RasterMask* tileMask = mask.isNull() ? nullptr : &mask;

    // The chain runs up to its end, or up to and including the accumulating operation,
    // which accumulates instead of applying; `end` is one past it.
    const bool finalPass = accumulation == nullptr;
    const size_t end = finalPass ? operations.size() : accumulation->position + 1;

    // Operation k writes the tile grown by the halos of the operations after it, up to `end`.
    std::vector<Rect64> outputRects(end);
    Rect64 sourceRect = tile;
    for (size_t k = end; k-- > 0;) {
        outputRects[k] = sourceRect;
        sourceRect = grow(sourceRect, operations[k]->halo());
    }

    PlaneBuffers current(sourceRect.width, sourceRect.height, plan_.planeTypes[0], plan_.channels);
    bytes += current.bytes();
    reader_.read(plan_.level, sourceRect, plan_.channels, current);
    Rect64 currentRect = sourceRect;

    // Auxiliary rasters are filled once per tile over the first operation's halo rect,
    // the largest rect of the chain, which holds every later operation's input rect.
    std::vector<PlaneBuffers> auxiliaryBuffers;
    auxiliaryBuffers.reserve(bindings_.auxiliaries.size());
    for (const auto& auxiliary : bindings_.auxiliaries) {
        std::vector<int32_t> channels;
        for (int32_t c = 0; c < std::max<int32_t>(auxiliary.second->planeCount(), 0); ++c) {
            channels.push_back(c);
        }
        auxiliaryBuffers.emplace_back(sourceRect.width, sourceRect.height,
                                      std::vector<DataType>(channels.size(), auxiliary.second->planeType()),
                                      channels);
        bytes += auxiliaryBuffers.back().bytes();
        BlockView whole = auxiliaryBuffers.back().view(plan_.level, sourceRect, plan_.levelScale);
        auxiliary.second->fillTile(whole);
    }

    TileObjectFilter objects(bindings_.objectSink, grid.tileRect(tileIndex), tileMask, *bindings_.counters);
    // An accumulation pass writes no sink: what the operations before the
    // accumulating one emit is dropped, seam fragments included.
    const SeamFragmentCallback dropFragment = [](const Polygon&, LayerId, const IntensityPartial*, int32_t,
                                                 int64_t) {};

    for (size_t k = 0; k < end; ++k) {
        token.throwIfCancelled();
        Operation& op = *operations[k];
        const bool accumulates = !finalPass && k + 1 == end;
        const Rect64& outputRect = outputRects[k];
        const BlockView inputView = current.view(plan_.level, currentRect, plan_.levelScale);
        // An accumulating operation writes nothing, so it gets no output buffer.
        std::optional<PlaneBuffers> output;
        BlockView outputView;
        if (!accumulates) {
            const std::vector<DataType>& outputTypes = plan_.planeTypes[k + 1];
            output.emplace(outputRect.width, outputRect.height, outputTypes,
                           outputChannels(current, outputTypes.size()));
            bytes += output->bytes();
            outputView = output->view(plan_.level, outputRect, plan_.levelScale);
        }

        // Each operation sees the auxiliaries over its own input rect: a sub-view of
        // the buffers filled once, over the first operation's halo rect.
        std::vector<BlockView> auxiliaryViews;
        auxiliaryViews.reserve(auxiliaryBuffers.size());
        for (PlaneBuffers& buffers : auxiliaryBuffers) {
            auxiliaryViews.push_back(subView(buffers, sourceRect, currentRect, plan_.level, plan_.levelScale));
        }

        OperationContextState state;
        if (finalPass) {
            state.objectSink = op.producesObjects() && bindings_.objectSink != nullptr ? &objects : nullptr;
            state.edgeSink = bindings_.edgeSink;
            state.annotationSink = bindings_.annotationSink;
            state.seamFragment = bindings_.seamFragment;
        } else {
            state.seamFragment = dropFragment;
        }
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
        state.pixelSizeMicrons = bindings_.pixelSizeMicrons;
        state.channels = &bindings_.spec->channels();
        state.cancellation = &token;
        state.mask = tileMask;
        state.tileIndex = tileIndex;

        // The operation's declarations decide the call shape; when it accumulates,
        // the call goes to the accumulator, a fresh instance of the same operation.
        const auto call = [&](OperationContext& context) {
            if (accumulates) {
                accumulation->accumulator->accumulate(context);
            } else {
                op.apply(context);
            }
        };
        if (runningOperation != nullptr) {
            *runningOperation = op.name();
        }
        if (op.arity() == ChannelArity::PerChannel) {
            for (int32_t plane = 0; plane < inputView.planeCount(); ++plane) {
                const BlockView in = singlePlane(inputView, plane);
                BlockView out = accumulates ? BlockView() : singlePlane(outputView, plane);
                OperationContextState planeState = state;
                planeState.input = &in;
                planeState.output = accumulates ? nullptr : &out;
                call(*makeOperationContext(std::move(planeState)));
            }
        } else {
            BlockView out = outputView;
            state.input = &inputView;
            state.output = accumulates ? nullptr : &out;
            call(*makeOperationContext(std::move(state)));
        }
        if (runningOperation != nullptr) {
            *runningOperation = nullptr;
        }
        if (accumulates) {
            return TileOutcome{false, bytes};
        }

        if (k + 1 < end) {
            zeroOutside(*output, outputRect, plan_.levelRect);
        }
        current = std::move(*output);
        currentRect = outputRect;
    }

    if (bindings_.rasterSink != nullptr) {
        const BlockView result = current.view(plan_.level, tile, plan_.levelScale);
        bindings_.rasterSink->writeTile(result);
    }
    return TileOutcome{false, bytes};
}

} // namespace slideio::processor::detail
