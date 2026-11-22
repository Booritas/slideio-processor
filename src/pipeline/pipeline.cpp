// Requirements: UR-EXE-1, UR-ALG-1, UR-ALG-5, UR-PIX-6, UR-IMG-5, UR-STG-3
#include <slideio/processor/pipeline/pipeline.hpp>
#include "core/detail/diagnostics.hpp"

#include <pipeline/detail/pipeline_impl.hpp>
#include <pipeline/detail/pipeline_scene_source.hpp>

#include <slideio/processor/core/error.hpp>
#include <slideio/processor/pipeline/executor.hpp>

#include <slideio/slideio/scenemetadata.hpp>
#include <slideio/slideio/scenesource.hpp>

#include <algorithm>
#include <atomic>
#include <exception>
#include <mutex>
#include <new>
#include <string>
#include <utility>

namespace slideio::processor {

namespace {

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

// The object sink of run(): counts what it is given and keeps nothing. Workers
// emit concurrently, so the count is atomic.
class DiscardingObjectSink : public ObjectSink {
public:
    void emit(const ObjectRecord&) override { ++emitted_; }
    void flush() override {}
    int64_t emittedCount() const override { return emitted_.load(); }

private:
    std::atomic<int64_t> emitted_{0};
};

// The lazy source behind levelCount(), rect(), readBlock() and asScene(): built once
// from a snapshot of the bindings, validating first when no plan is held. The build
// runs under the mutex, so concurrent first calls build it once; a build that throws
// leaves none, so the next call tries again. A binding call drops the source.
std::shared_ptr<detail::PipelineSceneSource> lazySource(const Pipeline& pipeline) {
    auto& impl = detail::PipelineAccess::impl(pipeline);
    std::lock_guard<std::mutex> lock(impl.lazyMutex);
    if (!impl.lazy) {
        impl.lazy = std::make_shared<detail::PipelineSceneSource>(detail::PipelineAccess::snapshot(pipeline));
    }
    return impl.lazy;
}

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
    impl_->bindingsChanged();
    impl_->budget = budget ? std::move(budget) : makeMemoryBudget(impl_->config.memoryCeilingBytes());
    return *this;
}

Pipeline& Pipeline::edgeSink(EdgeSink* sink) {
    impl_->bindingsChanged();
    impl_->edgeSink = sink;
    return *this;
}

Pipeline& Pipeline::annotationSink(AnnotationSink* sink) {
    impl_->bindingsChanged();
    impl_->annotationSink = sink;
    return *this;
}

Pipeline& Pipeline::rasterSink(RasterSink* sink) {
    impl_->bindingsChanged();
    impl_->rasterSink = sink;
    return *this;
}

Pipeline& Pipeline::auxiliaryRaster(const char* name, TileRasterSource* source) {
    requireName(name, "auxiliaryRaster");
    impl_->bindingsChanged();
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
    impl_->bindingsChanged();
    impl_->maskSource = source;
    return *this;
}

Pipeline& Pipeline::bindLayer(const char* name, LayerId layer) {
    requireName(name, "bindLayer");
    if (!layer.isValid()) {
        throw InvalidArgumentError("Pipeline::bindLayer: the layer identifier is invalid");
    }
    impl_->bindingsChanged();
    bindByName(impl_->layers, name, layer);
    return *this;
}

Pipeline& Pipeline::stageName(const char* name) {
    requireName(name, "stageName");
    impl_->bindingsChanged();
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

std::shared_ptr<slideio::Scene> Pipeline::asScene() const {
    std::shared_ptr<detail::PipelineSceneSource> source = lazySource(*this);
    const slideio::SceneMetadata metadata = source->metadata();
    try {
        return slideio::makeScene(std::move(source), metadata);
    } catch (const ProcessorError&) {
        throw;
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::exception& e) {
        // The description is built here, so SlideIO refusing it is a defect of this library.
        ErrorContext context;
        const std::string causeText = detail::diagnosticText(e);
        context.cause = causeText.c_str();
        throw InternalError("Pipeline::asScene: SlideIO refused the scene description", context);
    }
}

void Pipeline::runInto(ObjectSink& sink, ProgressObserver* progress, const CancellationToken& token) {
    validate();
    Impl& impl = *impl_;
    {
        // The pool lives for this run only: its destructor joins every thread, on
        // the failure path too.
        WorkerPool pool(impl.config.workerCount(), impl.budget);
        TileExecutor executor(impl.plan->grid, pool, impl.config);
        executor.run(*this, sink, progress, token);
        pool.shutdown();
    }

    // On success only, in this order.
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

int64_t Pipeline::truncatedObjectCount() const noexcept { return impl_->counters.truncatedObjects.load(); }

int32_t Pipeline::levelCount() const { return lazySource(*this)->levelCount(); }

Rect64 Pipeline::rect(int32_t level) const { return lazySource(*this)->rect(level); }

void Pipeline::readBlock(int32_t level, const Rect64& region, BlockView& out) const {
    lazySource(*this)->readBlock(level, region, out);
}

void Pipeline::validate() const {
    Impl& impl = *impl_;
    impl.plan.reset();  // Drops the pointer only: a failed validate() leaves no plan.
    if (!impl.reader) {
        // Built together, so a failure leaves none of them set. The reader
        // describes the scene first, reporting a SlideIO failure as IoError.
        auto reader = std::make_shared<const detail::SceneReader>(impl.scene);
        std::shared_ptr<const LevelSelector> selector;
        try {
            selector = std::make_shared<const LevelSelector>(impl.scene);
        } catch (const InternalError& e) {
            // LevelSelector reports a SlideIO failure as InternalError; on this
            // path it is a failure to describe the scene, so IoError.
            ErrorContext context;
            context.cause = e.cause()[0] != '\0' ? e.cause() : e.what();
            throw IoError("Pipeline::validate: SlideIO failed to describe the scene", context);
        }
        impl.levels = reader->levels();
        impl.selector = std::move(selector);
        impl.reader = std::move(reader);
    }
    const std::vector<DataType> channelTypes = impl.reader->channelTypes();
    detail::RunPlanInputs inputs{&impl.spec,          &impl.config,  &channelTypes,
                                 impl.selector.get(), &impl.levels,  &impl.layers,
                                 &impl.auxiliaries,   impl.maskSource != nullptr,
                                 impl.budget->ceilingBytes()};
    // A new plan is swapped in; a plan a run or a snapshot still holds is never touched.
    impl.plan = std::make_shared<const detail::RunPlan>(detail::buildRunPlan(inputs));
}

} // namespace slideio::processor
