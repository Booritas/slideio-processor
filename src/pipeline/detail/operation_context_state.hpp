// Requirements: UR-EXT-1, UR-EXT-3, UR-EXT-4
#pragma once

#include <slideio/processor/pipeline/operation.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace slideio::processor::detail {

/// Everything an OperationContext reports, as plain members (ruling P6). Every
/// pointer is borrowed from the run and must outlive the context built from it.
/// A null pointer reads as the documented fallback: a null view, a null sink,
/// ChannelSelection::all(), a never-cancelled token, no mask.
struct OperationContextState {
    const BlockView* input = nullptr;
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
    const ChannelSelection* channels = nullptr;
    const CancellationToken* cancellation = nullptr;
    const RasterMask* mask = nullptr;
    /// The seam merger's entry point; empty when the run has none (plan C supplies it).
    std::function<void(const Polygon&, LayerId, const IntensityPartial*, int32_t)> seamFragment;
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
