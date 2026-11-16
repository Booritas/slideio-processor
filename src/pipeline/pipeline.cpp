// Requirements: UR-EXE-1, UR-ALG-1, UR-ALG-5, UR-PIX-6, UR-IMG-5, UR-STG-3
#include <slideio/processor/pipeline/pipeline.hpp>

#include <pipeline/detail/pipeline_impl.hpp>

#include <slideio/processor/core/error.hpp>

#include <algorithm>
#include <string>
#include <utility>

namespace slideio::processor {

namespace {

[[noreturn]] void notUntilPlanC() { throw InternalError("not implemented until plan C"); }

void requireName(const char* name, const char* method) {
    if (name == nullptr || name[0] == '\0') {
        throw InvalidArgumentError((std::string("Pipeline::") + method + ": the name is null or empty").c_str());
    }
}

// Sets or replaces the binding of `name`.
template <typename T>
void bindByName(std::vector<std::pair<std::string, T>>& bindings, const char* name, T value) {
    const auto found = std::find_if(bindings.begin(), bindings.end(),
                                    [name](const std::pair<std::string, T>& b) { return b.first == name; });
    if (found != bindings.end()) {
        found->second = value;
    } else {
        bindings.emplace_back(name, value);
    }
}

// The object sink of run() (ruling P29): counts what it is given and keeps nothing.
class DiscardingObjectSink : public ObjectSink {
public:
    void emit(const ObjectRecord&) override { ++emitted_; }
    void flush() override {}
    int64_t emittedCount() const override { return emitted_; }

private:
    int64_t emitted_ = 0;
};

} // namespace

Pipeline::Pipeline(std::shared_ptr<slideio::Scene> scene, const Configuration& config, const PipelineSpec& spec) {
    if (!scene) {
        throw InvalidArgumentError("Pipeline: the scene is null");
    }
    impl_ = std::make_unique<Impl>(std::move(scene), config, spec);
    impl_->budget = makeMemoryBudget(impl_->config.memoryCeilingBytes());
}

Pipeline::~Pipeline() = default;

Pipeline& Pipeline::memoryBudget(MemoryBudgetPtr budget) {
    impl_->plan.reset();
    impl_->budget = budget ? std::move(budget) : makeMemoryBudget(impl_->config.memoryCeilingBytes());
    return *this;
}

Pipeline& Pipeline::edgeSink(EdgeSink* sink) {
    impl_->plan.reset();
    impl_->edgeSink = sink;
    return *this;
}

Pipeline& Pipeline::annotationSink(AnnotationSink* sink) {
    impl_->plan.reset();
    impl_->annotationSink = sink;
    return *this;
}

Pipeline& Pipeline::rasterSink(RasterSink* sink) {
    impl_->plan.reset();
    impl_->rasterSink = sink;
    return *this;
}

Pipeline& Pipeline::auxiliaryRaster(const char* name, TileRasterSource* source) {
    requireName(name, "auxiliaryRaster");
    impl_->plan.reset();
    if (source == nullptr) {
        auto& bindings = impl_->auxiliaries;
        bindings.erase(std::remove_if(bindings.begin(), bindings.end(),
                                      [name](const std::pair<std::string, TileRasterSource*>& b) {
                                          return b.first == name;
                                      }),
                       bindings.end());
    } else {
        bindByName(impl_->auxiliaries, name, source);
    }
    return *this;
}

Pipeline& Pipeline::maskSource(TileMaskSource* source) {
    impl_->plan.reset();
    impl_->maskSource = source;
    return *this;
}

Pipeline& Pipeline::bindLayer(const char* name, LayerId layer) {
    requireName(name, "bindLayer");
    if (!layer.isValid()) {
        throw InvalidArgumentError("Pipeline::bindLayer: the layer identifier is invalid");
    }
    impl_->plan.reset();
    bindByName(impl_->layers, name, layer);
    return *this;
}

Pipeline& Pipeline::stageName(const char* name) {
    requireName(name, "stageName");
    impl_->plan.reset();
    impl_->stageName = name;
    return *this;
}

const PipelineSpec& Pipeline::spec() const noexcept { return impl_->spec; }

int32_t Pipeline::analysisLevel() const {
    if (!impl_->plan) {
        throw InvalidArgumentError("Pipeline::analysisLevel: call validate() first");
    }
    return impl_->plan->level;
}

std::shared_ptr<slideio::Scene> Pipeline::asScene() const { notUntilPlanC(); }

void Pipeline::runInto(ObjectSink& sink, ProgressObserver* progress, const CancellationToken& token) {
    validate();
    Impl& impl = *impl_;
    const detail::RunPlan& plan = *impl.plan;
    if (!plan.globalContextPositions.empty()) {
        throw InternalError("Pipeline::runInto: global-context operations are not run yet");
    }
    impl.counters.reset();

    detail::Bindings bindings;
    bindings.objectSink = &sink;
    bindings.edgeSink = impl.edgeSink;
    bindings.annotationSink = impl.annotationSink;
    bindings.rasterSink = impl.rasterSink;
    bindings.auxiliaries = impl.auxiliaries;
    bindings.maskSource = impl.maskSource;
    bindings.spec = &impl.spec;
    bindings.selector = impl.selector.get();
    bindings.counters = &impl.counters;
    const detail::TileEvaluator evaluator(plan, *impl.reader, std::move(bindings));

    // One worker, tiles in ascending order.
    const int64_t total = plan.grid.tileCount();
    for (int64_t tile = 0; tile < total; ++tile) {
        evaluator.evaluate(tile, token);
        if (progress != nullptr) {
            progress->onProgress(impl.stageName.c_str(), static_cast<double>(tile + 1) / static_cast<double>(total),
                                 tile + 1, total);
        }
    }

    // On success only, in this order (ruling P27).
    sink.flush();
    if (impl.edgeSink != nullptr) {
        impl.edgeSink->flush();
    }
    if (impl.annotationSink != nullptr) {
        impl.annotationSink->flush();
    }
    if (impl.rasterSink != nullptr) {
        impl.rasterSink->flush();
    }
}

void Pipeline::run(ProgressObserver* progress, const CancellationToken& token) {
    DiscardingObjectSink discard;
    runInto(discard, progress, token);
}

int32_t Pipeline::levelCount() const { notUntilPlanC(); }

Rect64 Pipeline::rect(int32_t level) const {
    (void)level;
    notUntilPlanC();
}

void Pipeline::readBlock(int32_t level, const Rect64& region, BlockView& out) const {
    (void)level;
    (void)region;
    (void)out;
    notUntilPlanC();
}

void Pipeline::validate() const {
    Impl& impl = *impl_;
    impl.plan.reset();
    if (!impl.reader) {
        // Built together, so a failure leaves none of them set.
        auto reader = std::make_unique<detail::SceneReader>(impl.scene);
        auto selector = std::make_unique<LevelSelector>(impl.scene);
        impl.levels = detail::levelTable(*impl.scene);
        impl.selector = std::move(selector);
        impl.reader = std::move(reader);
    }
    const std::vector<DataType> channelTypes = impl.reader->channelTypes();
    detail::RunPlanInputs inputs{&impl.spec,       &impl.config,          impl.scene,
                                 &channelTypes,    impl.selector.get(),   &impl.levels,
                                 &impl.layers,     &impl.auxiliaries,     impl.maskSource != nullptr,
                                 impl.budget->ceilingBytes()};
    impl.plan = std::make_unique<detail::RunPlan>(detail::buildRunPlan(inputs));
}

} // namespace slideio::processor
