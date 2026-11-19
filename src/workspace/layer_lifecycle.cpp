// Requirements: UR-STG-10, UR-STG-12, UR-RES-16
#include <slideio/processor/workspace/layer_lifecycle.hpp>

namespace slideio::processor {

class LayerLifecycle::Impl {
public:
    explicit Impl(LayerRegistry& registryIn) noexcept : registry(registryIn) {}
    LayerRegistry& registry;
};

LayerLifecycle::LayerLifecycle(LayerRegistry& registry) noexcept : impl_(new Impl(registry)) {}

LayerLifecycle::~LayerLifecycle() = default;

LayerId LayerLifecycle::declareSemantic(const char* name, LayerId parent) {
    return impl_->registry.declareSemantic(name, parent);
}

LayerId LayerLifecycle::declarePrimitive(const char* name) { return impl_->registry.declarePrimitive(name); }

void LayerLifecycle::drop(LayerId primitiveLayer) { impl_->registry.dropPrimitive(primitiveLayer); }

bool LayerLifecycle::isDropped(LayerId layer) const noexcept { return impl_->registry.isDropped(layer); }

bool LayerLifecycle::wasEverPopulated(LayerId layer) const noexcept {
    return impl_->registry.wasEverPopulated(layer);
}

int64_t LayerLifecycle::droppedObjectCount(LayerId layer) const noexcept {
    return impl_->registry.droppedObjectCount(layer);
}

} // namespace slideio::processor
