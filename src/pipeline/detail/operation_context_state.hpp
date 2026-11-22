// Requirements: UR-EXT-1, UR-EXT-3, UR-EXT-4
#pragma once

#include <slideio/processor/pipeline/operation.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace slideio::processor::detail {

/// The seam merger's entry point: a fragment, its layer, its intensity partials
/// (`count` of them, null when none), its join key (fragments join only with
/// fragments of an equal key) and the index of the tile that emitted it.
using SeamFragmentCallback =
    std::function<void(const Polygon&, LayerId, const IntensityPartial*, int32_t count, int64_t joinKey,
                       int64_t tileIndex)>;

/// Everything an OperationContext reports, as plain members. Every
/// pointer is borrowed from the run and must outlive the context built from it.
/// A null pointer reads as the documented fallback: a null view, a null sink,
/// ChannelSelection::all(), a never-cancelled token, no mask.
struct OperationContextState {
    const BlockView* input = nullptr;
    /// The scene's samples over the same rect as `input`, every selected channel;
    /// null reads as `input`.
    const BlockView* sourceInput = nullptr;
    BlockView* output = nullptr;
    ObjectSink* objectSink = nullptr;
    EdgeSink* edgeSink = nullptr;
    AnnotationSink* annotationSink = nullptr;
    std::vector<std::pair<std::string, const BlockView*>> auxiliaries;
    LayerId targetLayer;
    int32_t level = 0;
    Rect64 tileRectAtLevel{0, 0, 0, 0}, haloRectAtLevel{0, 0, 0, 0}, tileRect{0, 0, 0, 0},
        haloRect{0, 0, 0, 0};
    double levelScale = 1.0;
    /// The scene's level-0 pixel size in micrometres; 0 when unknown.
    double pixelSizeMicrons = 0.0;
    const ChannelSelection* channels = nullptr;
    const CancellationToken* cancellation = nullptr;
    const RasterMask* mask = nullptr;
    /// The index in the run's grid of the tile being evaluated; handed to seamFragment.
    int64_t tileIndex = -1;
    /// The seam merger's entry point; empty when the run has none.
    SeamFragmentCallback seamFragment;
    /// The run's total of objects operations dropped as truncated; null in a pass or
    /// a lazy read that keeps no count, where countTruncatedObjects() validates and drops.
    std::atomic<int64_t>* truncatedCounter = nullptr;
};

/// Builds the context the framework hands to Operation::apply().
std::unique_ptr<OperationContext> makeOperationContext(OperationContextState state);

/// The one way into OperationContext's private constructor and state.
struct OperationContextAccess {
    static std::unique_ptr<OperationContext> make(OperationContextState state);
};

} // namespace slideio::processor::detail

namespace slideio::processor {

class OperationContext::Impl {
public:
    detail::OperationContextState state;
};

} // namespace slideio::processor
