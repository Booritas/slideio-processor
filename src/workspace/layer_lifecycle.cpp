// Requirements: UR-STG-10, UR-STG-12, UR-RES-16
#include <slideio/processor/workspace/layer_lifecycle.hpp>

namespace slideio::processor {

LayerLifecycle::LayerLifecycle(LayerRegistry& registry) noexcept : registry_(registry) {}

LayerLifecycle::~LayerLifecycle() = default;

LayerId LayerLifecycle::declareSemantic(const char* name, LayerId parent) {
    return registry_.declareSemantic(name, parent);
}

LayerId LayerLifecycle::declarePrimitive(const char* name) { return registry_.declarePrimitive(name); }

void LayerLifecycle::drop(LayerId primitiveLayer) { registry_.dropPrimitive(primitiveLayer); }

bool LayerLifecycle::isDropped(LayerId layer) const noexcept { return registry_.isDropped(layer); }

bool LayerLifecycle::wasEverPopulated(LayerId layer) const noexcept { return registry_.wasEverPopulated(layer); }

int64_t LayerLifecycle::droppedObjectCount(LayerId layer) const noexcept {
    return registry_.droppedObjectCount(layer);
}

} // namespace slideio::processor
