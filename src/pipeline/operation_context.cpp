// Requirements: UR-EXT-1, UR-EXT-3, UR-EXT-4, UR-SEG-4
#include <slideio/processor/pipeline/operation.hpp>

#include <slideio/processor/core/error.hpp>

#include <core/detail/level_mapping.hpp>
#include <pipeline/detail/operation_context_state.hpp>

#include <cstring>
#include <vector>

namespace slideio::processor {

namespace {
// Fallbacks for what the run did not supply (OperationContextState's null pointers).
const BlockView& nullInput() noexcept {
    static const BlockView view;
    return view;
}
const ChannelSelection& allChannels() noexcept {
    static const ChannelSelection all = ChannelSelection::all();
    return all;
}
const CancellationToken& neverCancelled() noexcept {
    static const CancellationToken token;
    return token;
}
} // namespace

OperationContext::OperationContext() : impl_(std::make_unique<Impl>()) {}

OperationContext::~OperationContext() = default;

const BlockView& OperationContext::input() const noexcept {
    return impl_->state.input != nullptr ? *impl_->state.input : nullInput();
}

const BlockView& OperationContext::sourceInput() const noexcept {
    return impl_->state.sourceInput != nullptr ? *impl_->state.sourceInput : input();
}

BlockView& OperationContext::output() noexcept {
    if (impl_->state.output != nullptr) {
        return *impl_->state.output;
    }
    // One null view per thread, reset on every call, so an operation that
    // assigns to it disturbs no other tile and no later call.
    thread_local BlockView nullOutput;
    nullOutput = BlockView();
    return nullOutput;
}

ObjectSink* OperationContext::objectSink() const noexcept {
    return impl_->state.objectSink;
}

EdgeSink* OperationContext::edgeSink() const noexcept {
    return impl_->state.edgeSink;
}

AnnotationSink* OperationContext::annotationSink() const noexcept {
    return impl_->state.annotationSink;
}

const BlockView* OperationContext::auxiliary(const char* name) const noexcept {
    if (name == nullptr) {
        return nullptr;
    }
    for (const auto& entry : impl_->state.auxiliaries) {
        if (std::strcmp(entry.first.c_str(), name) == 0) {
            return entry.second;
        }
    }
    return nullptr;
}

LayerId OperationContext::targetLayer() const noexcept {
    return impl_->state.targetLayer;
}

int32_t OperationContext::level() const noexcept {
    return impl_->state.level;
}

Rect64 OperationContext::tileRectAtLevel() const noexcept {
    return impl_->state.tileRectAtLevel;
}

Rect64 OperationContext::haloRectAtLevel() const noexcept {
    return impl_->state.haloRectAtLevel;
}

Rect64 OperationContext::tileRect() const noexcept {
    return impl_->state.tileRect;
}

Rect64 OperationContext::haloRect() const noexcept {
    return impl_->state.haloRect;
}

double OperationContext::levelScale() const noexcept {
    return impl_->state.levelScale;
}

double OperationContext::pixelSizeMicrons() const noexcept {
    return impl_->state.pixelSizeMicrons;
}

Point64 OperationContext::toLevel0(Point64 levelPoint) const noexcept {
    return Point64{detail::levelToLevel0(levelPoint.x, impl_->state.levelScale),
                   detail::levelToLevel0(levelPoint.y, impl_->state.levelScale)};
}

Polygon OperationContext::toLevel0(const Polygon& levelPolygon) const {
    Polygon mapped;
    std::vector<Point64> points;
    for (int32_t ring = 0; ring < levelPolygon.ringCount(); ++ring) {
        points.resize(static_cast<size_t>(levelPolygon.ringPointCount(ring)));
        levelPolygon.ringPoints(ring, points.data(), static_cast<int64_t>(points.size()));
        for (Point64& p : points) {
            p = toLevel0(p);
        }
        mapped.addRing(points.data(), static_cast<int64_t>(points.size()), levelPolygon.ringIsHole(ring));
    }
    return mapped;
}

const ChannelSelection& OperationContext::channels() const noexcept {
    return impl_->state.channels != nullptr ? *impl_->state.channels : allChannels();
}

const CancellationToken& OperationContext::cancellation() const noexcept {
    return impl_->state.cancellation != nullptr ? *impl_->state.cancellation : neverCancelled();
}

const RasterMask* OperationContext::mask() const noexcept {
    return impl_->state.mask;
}

void OperationContext::countTruncatedObjects(int64_t count) {
    if (count < 0) {
        throw InvalidArgumentError("countTruncatedObjects: the count must not be negative");
    }
    if (impl_->state.truncatedCounter != nullptr) {
        impl_->state.truncatedCounter->fetch_add(count);
    }
}

void OperationContext::emitSeamFragment(const Polygon& fragment, LayerId layer) {
    emitSeamFragment(fragment, layer, nullptr, 0);
}

void OperationContext::emitSeamFragment(const Polygon& fragment, LayerId layer,
                                        const IntensityPartial* partials, int32_t partialCount) {
    emitSeamFragment(fragment, layer, partials, partialCount, 0);
}

void OperationContext::emitSeamFragment(const Polygon& fragment, LayerId layer,
                                        const IntensityPartial* partials, int32_t partialCount, int64_t joinKey) {
    if (partialCount < 0) {
        throw InvalidArgumentError("emitSeamFragment: partialCount must not be negative");
    }
    if (partials == nullptr && partialCount > 0) {
        throw InvalidArgumentError("emitSeamFragment: partials must not be null when partialCount is positive");
    }
    if (!impl_->state.seamFragment) {
        throw InternalError("emitSeamFragment: this run has no seam merger");
    }
    impl_->state.seamFragment(fragment, layer, partials, partialCount, joinKey, impl_->state.tileIndex);
}

} // namespace slideio::processor
