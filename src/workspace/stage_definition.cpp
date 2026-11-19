// Requirements: UR-STG-2, UR-STG-3, UR-STG-4, UR-STG-10, UR-IMG-5
#include <slideio/processor/workspace/stage_definition.hpp>

#include <workspace/detail/stage_record.hpp>

#include <slideio/processor/core/error.hpp>

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace slideio::processor {

namespace {

[[noreturn]] void fail(const std::string& what) { throw InvalidArgumentError(("StageDefinition: " + what).c_str()); }

void requireText(const char* text, const char* what) {
    if (text == nullptr || text[0] == '\0') {
        fail(std::string(what) + " is null or empty");
    }
}

void requireIndex(int32_t index, size_t count, const char* method) {
    if (index < 0 || static_cast<size_t>(index) >= count) {
        fail(std::string(method) + ": index " + std::to_string(index) + " is out of range [0, " +
             std::to_string(count) + ")");
    }
}

template <typename T>
void addOnce(std::vector<T>& list, const T& value) {
    if (std::find(list.begin(), list.end(), value) == list.end()) {
        list.push_back(value);
    }
}

} // namespace

class StageDefinition::Impl {
public:
    struct LayerRaster {
        std::string binding;
        LayerId layer;
        LayerRasterContent content = LayerRasterContent::Label;
        bool operator==(const LayerRaster& o) const {
            return binding == o.binding && layer == o.layer && content == o.content;
        }
    };
    struct RasterRead {
        std::string binding;
        RasterId raster;
        bool operator==(const RasterRead& o) const { return binding == o.binding && raster == o.raster; }
    };

    // Adds to the union of everything read, which keeps first-declaration order.
    void noteRead(LayerId layer) { addOnce(allReads, layer); }

    // A binding name names one input across both raster lists, since a runner resolves
    // bindings by name. Repeating an entry exactly is a no-op; reusing its name for
    // another target throws, naming the stage and the binding.
    template <typename T>
    void addBinding(std::vector<T>& list, const T& entry) {
        const bool rasterTaken = std::any_of(rasterReads.begin(), rasterReads.end(),
                                             [&](const RasterRead& r) { return r.binding == entry.binding; });
        const bool layerTaken = std::any_of(layerRasters.begin(), layerRasters.end(),
                                            [&](const LayerRaster& r) { return r.binding == entry.binding; });
        if ((rasterTaken || layerTaken) && std::find(list.begin(), list.end(), entry) == list.end()) {
            fail("stage '" + name + "' already binds '" + entry.binding + "' to another input");
        }
        addOnce(list, entry);
    }

    std::string name;
    PipelineSpec spec;
    std::vector<LayerId> explicitReads;
    std::vector<LayerId> allReads;
    std::vector<LayerId> writes;
    std::vector<LayerId> annotates;
    std::vector<LayerRaster> layerRasters;
    std::vector<RasterRead> rasterReads;
    std::vector<std::string> dependencies;
    bool hasRestriction = false;
    LayerId restriction;
    bool hasWrittenRaster = false;
    RasterId writtenRaster;
};

StageDefinition::StageDefinition(const char* name) : impl_(new Impl) {
    requireText(name, "the stage name");
    impl_->name = name;
}

StageDefinition::StageDefinition(const StageDefinition& other) : impl_(new Impl(*other.impl_)) {}

StageDefinition& StageDefinition::operator=(const StageDefinition& other) {
    if (this != &other) {
        std::unique_ptr<Impl> copy(new Impl(*other.impl_));
        impl_ = std::move(copy);
    }
    return *this;
}

StageDefinition::~StageDefinition() = default;

StageDefinition& StageDefinition::readsLayer(LayerId layer) {
    addOnce(impl_->explicitReads, layer);
    impl_->noteRead(layer);
    return *this;
}

StageDefinition& StageDefinition::writesLayer(LayerId layer) {
    addOnce(impl_->writes, layer);
    return *this;
}

StageDefinition& StageDefinition::annotatesLayer(LayerId layer) {
    addOnce(impl_->annotates, layer);
    impl_->noteRead(layer);
    return *this;
}

StageDefinition& StageDefinition::dependsOn(const char* stageName) {
    requireText(stageName, "a dependency name");
    addOnce(impl_->dependencies, std::string(stageName));
    return *this;
}

StageDefinition& StageDefinition::pipeline(const PipelineSpec& spec) {
    impl_->spec = spec;
    return *this;
}

StageDefinition& StageDefinition::writesRaster(RasterId raster) {
    if (impl_->hasWrittenRaster) {
        fail("stage '" + impl_->name + "' already declares a written raster; a stage writes at most one");
    }
    impl_->hasWrittenRaster = true;
    impl_->writtenRaster = raster;
    return *this;
}

StageDefinition& StageDefinition::readsRaster(const char* bindingName, RasterId raster) {
    requireText(bindingName, "a raster binding name");
    impl_->addBinding(impl_->rasterReads, Impl::RasterRead{bindingName, raster});
    return *this;
}

StageDefinition& StageDefinition::readsLayerAsRaster(const char* bindingName, LayerId layer,
                                                     LayerRasterContent content) {
    requireText(bindingName, "a layer-raster binding name");
    impl_->addBinding(impl_->layerRasters, Impl::LayerRaster{bindingName, layer, content});
    impl_->noteRead(layer);
    return *this;
}

StageDefinition& StageDefinition::restrictToLayer(LayerId layer) {
    Impl& i = *impl_;
    if (i.hasRestriction && i.restriction != layer) {
        // The replaced restriction stops being read unless another declaration still reads it.
        const LayerId old = i.restriction;
        const bool stillRead =
            std::find(i.explicitReads.begin(), i.explicitReads.end(), old) != i.explicitReads.end() ||
            std::find(i.annotates.begin(), i.annotates.end(), old) != i.annotates.end() ||
            std::any_of(i.layerRasters.begin(), i.layerRasters.end(),
                        [old](const Impl::LayerRaster& r) { return r.layer == old; });
        if (!stillRead) {
            i.allReads.erase(std::remove(i.allReads.begin(), i.allReads.end(), old), i.allReads.end());
        }
    }
    i.hasRestriction = true;
    i.restriction = layer;
    i.noteRead(layer);
    return *this;
}

const char* StageDefinition::name() const noexcept { return impl_->name.c_str(); }

const PipelineSpec& StageDefinition::pipeline() const noexcept { return impl_->spec; }

int32_t StageDefinition::readLayerCount() const noexcept { return static_cast<int32_t>(impl_->allReads.size()); }

LayerId StageDefinition::readLayerAt(int32_t index) const {
    requireIndex(index, impl_->allReads.size(), "readLayerAt");
    return impl_->allReads[static_cast<size_t>(index)];
}

int32_t StageDefinition::writtenLayerCount() const noexcept { return static_cast<int32_t>(impl_->writes.size()); }

LayerId StageDefinition::writtenLayerAt(int32_t index) const {
    requireIndex(index, impl_->writes.size(), "writtenLayerAt");
    return impl_->writes[static_cast<size_t>(index)];
}

int32_t StageDefinition::annotatedLayerCount() const noexcept {
    return static_cast<int32_t>(impl_->annotates.size());
}

LayerId StageDefinition::annotatedLayerAt(int32_t index) const {
    requireIndex(index, impl_->annotates.size(), "annotatedLayerAt");
    return impl_->annotates[static_cast<size_t>(index)];
}

bool StageDefinition::writesAnyRaster() const noexcept { return impl_->hasWrittenRaster; }

RasterId StageDefinition::writtenRaster() const noexcept { return impl_->writtenRaster; }

int32_t StageDefinition::readRasterCount() const noexcept { return static_cast<int32_t>(impl_->rasterReads.size()); }

const char* StageDefinition::readRasterBindingAt(int32_t index) const {
    requireIndex(index, impl_->rasterReads.size(), "readRasterBindingAt");
    return impl_->rasterReads[static_cast<size_t>(index)].binding.c_str();
}

RasterId StageDefinition::readRasterAt(int32_t index) const {
    requireIndex(index, impl_->rasterReads.size(), "readRasterAt");
    return impl_->rasterReads[static_cast<size_t>(index)].raster;
}

int32_t StageDefinition::rasterLayerCount() const noexcept { return static_cast<int32_t>(impl_->layerRasters.size()); }

const char* StageDefinition::rasterBindingAt(int32_t index) const {
    requireIndex(index, impl_->layerRasters.size(), "rasterBindingAt");
    return impl_->layerRasters[static_cast<size_t>(index)].binding.c_str();
}

LayerId StageDefinition::rasterLayerAt(int32_t index) const {
    requireIndex(index, impl_->layerRasters.size(), "rasterLayerAt");
    return impl_->layerRasters[static_cast<size_t>(index)].layer;
}

LayerRasterContent StageDefinition::rasterContentAt(int32_t index) const {
    requireIndex(index, impl_->layerRasters.size(), "rasterContentAt");
    return impl_->layerRasters[static_cast<size_t>(index)].content;
}

int32_t StageDefinition::explicitDependencyCount() const noexcept {
    return static_cast<int32_t>(impl_->dependencies.size());
}

const char* StageDefinition::explicitDependencyAt(int32_t index) const {
    requireIndex(index, impl_->dependencies.size(), "explicitDependencyAt");
    return impl_->dependencies[static_cast<size_t>(index)].c_str();
}

bool StageDefinition::hasLayerRestriction() const noexcept { return impl_->hasRestriction; }

LayerId StageDefinition::layerRestriction() const noexcept { return impl_->restriction; }

namespace detail {

int32_t StageDefinitionAccess::explicitReadCount(const StageDefinition& definition) noexcept {
    return static_cast<int32_t>(definition.impl_->explicitReads.size());
}

LayerId StageDefinitionAccess::explicitReadAt(const StageDefinition& definition, int32_t index) {
    requireIndex(index, definition.impl_->explicitReads.size(), "explicitReadAt");
    return definition.impl_->explicitReads[static_cast<size_t>(index)];
}

} // namespace detail

} // namespace slideio::processor
