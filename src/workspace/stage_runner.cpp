// Requirements: UR-STG-3, UR-STG-5, UR-STG-6, UR-STG-7, UR-STG-10, UR-EXE-1, UR-EXE-4, UR-EXE-5
#include <slideio/processor/workspace/stage_runner.hpp>

#include <workspace/detail/run_fault.hpp>
#include <workspace/detail/run_slot.hpp>

#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/flow.hpp>
#include <slideio/processor/core/log.hpp>
#include <slideio/processor/pipeline/operation_catalog.hpp>
#include <slideio/processor/pipeline/operation_parameters.hpp>
#include <slideio/processor/pipeline/pipeline.hpp>
#include <slideio/processor/pipeline/pipeline_spec.hpp>
#include <slideio/processor/pipeline/tile_grid.hpp>
#include <slideio/processor/storage/annotation_store.hpp>
#include <slideio/processor/storage/edge_store.hpp>
#include <slideio/processor/storage/layer_registry.hpp>
#include <slideio/processor/storage/object_store.hpp>
#include <slideio/processor/storage/raster_store.hpp>
#include <slideio/processor/storage/stage_state_reader.hpp>
#include <slideio/processor/storage/stage_state_writer.hpp>
#include <slideio/processor/storage/store.hpp>
#include <slideio/processor/workspace/stage_definition.hpp>
#include <slideio/processor/workspace/stage_registry.hpp>

#include <atomic>
#include <cstring>
#include <exception>
#include <new>
#include <string>
#include <vector>

namespace slideio::processor {

namespace detail {

RunSlot::RunSlot(Workspace& workspace, const char* stageName) : workspace_(workspace) {
    if (!takeRunSlot(workspace)) {
        ErrorContext context;
        context.stageName = stageName;
        throw StageError((std::string("stage \"") + stageName +
                          "\" cannot run: another stage is running in this workspace")
                             .c_str(),
                         context);
    }
}

RunSlot::~RunSlot() { releaseRunSlot(workspace_); }

} // namespace detail

namespace {

[[noreturn]] void throwStage(const std::string& stage, const std::string& message) {
    ErrorContext context;
    context.stageName = stage.c_str();
    throw StageError(message.c_str(), context);
}

std::atomic<bool> gFailNextRecording{false};
std::atomic<bool> gFailNextRerunDiscard{false};

} // namespace

void detail::failNextStageFailureRecordingForTesting() noexcept { gFailNextRecording.store(true); }

void detail::failNextRerunDiscardForTesting() noexcept { gFailNextRerunDiscard.store(true); }

class StageRunner::Impl {
public:
    Impl(Workspace& workspaceIn, StageId stageIn) : workspace(workspaceIn), stage(stageIn) {
        StageRegistry& registry = workspace.stages();
        registry.isUnreadable(stage);  // throws InvalidArgumentError for an unknown stage
        name = registry.name(stage);
        rerun = registry.status(stage) != StageStatus::Pending;
    }

    // The first refusal; it needs no run slot.
    void checkWritable() const {
        if (workspace.isReadOnly()) {
            throw IoError(("stage \"" + name + "\" cannot run: the workspace is read-only").c_str());
        }
    }

    // The refusals made while the caller holds the run slot, in §4.4's order. Each leaves
    // the stage as it was: nothing has been written yet.
    void gate() const {
        const StageRegistry& registry = workspace.stages();
        if (registry.isUnreadable(stage)) {
            throwStage(name, "stage \"" + name + "\" cannot run: its stored record could not be read");
        }
        const int32_t count = registry.dependencyCount(stage);
        for (int32_t i = 0; i < count; ++i) {
            const StageId dependency = registry.dependencyAt(stage, i);
            const bool complete = registry.status(dependency) == StageStatus::Complete;
            if (!complete || registry.isStale(dependency)) {
                const std::string dependencyName = registry.name(dependency);
                throwStage(name, "stage \"" + name + "\" cannot run: its dependency \"" + dependencyName +
                                     (complete ? "\" is stale" : "\" is not complete"));
            }
        }
        checkLayerBinding(registry.definition(stage));
    }

    // Decision 6: every operation that produces objects names one of the stage's written
    // layers, or leaves it out when the stage writes exactly one. Raster-only operations are
    // not consulted. Part of gate(): it needs only the definition, so a bad definition is
    // refused before anything is written, and the refusal names the stage.
    void checkLayerBinding(const StageDefinition& definition) const {
        const PipelineSpec spec = definition.pipeline();
        const LayerRegistry& layers = workspace.layers();
        for (int32_t i = 0; i < spec.operationCount(); ++i) {
            const char* operation = spec.operationName(i);
            // An unknown operation is Pipeline::validate()'s refusal, inside the run.
            const OperationCatalog& catalog = OperationCatalog::instance();
            if (!catalog.isRegistered(operation) || !catalog.producesObjects(operation)) continue;
            const OperationParameters& parameters = spec.operationParameters(i);
            bool present = false;
            for (int32_t k = 0; k < parameters.count(); ++k) {
                present = present || std::strcmp(parameters.keyAt(k), "targetLayer") == 0;
            }
            const std::string target = present ? parameters.getString("targetLayer", "") : "";
            if (!target.empty()) {
                bool declared = false;
                for (int32_t k = 0; k < definition.writtenLayerCount(); ++k) {
                    declared = declared || target == layers.name(definition.writtenLayerAt(k));
                }
                if (!declared) {
                    throwBinding(operation, "targets layer \"" + target + "\", which the stage does not write");
                }
            } else if (definition.writtenLayerCount() != 1) {
                throwBinding(operation, "has no target layer and the stage writes " +
                                            std::to_string(definition.writtenLayerCount()) +
                                            " layers; name one with its \"targetLayer\" parameter");
            }
        }
    }

    [[noreturn]] void throwBinding(const char* operation, const std::string& what) const {
        ErrorContext context;
        context.stageName = name.c_str();
        context.operationName = operation;
        throw StageError(("stage \"" + name + "\": operation " + operation + " " + what).c_str(), context);
    }

    // Decision 3, first step of a re-run: every stage that depends directly on this one is
    // marked stale, whatever this run's outcome, before anything of this stage changes.
    void markDependentsStale(StageRegistry& registry) {
        for (int32_t i = 0; i < registry.count(); ++i) {
            const StageId other = registry.at(i);
            const int32_t dependencies = registry.dependencyCount(other);
            for (int32_t k = 0; k < dependencies; ++k) {
                if (registry.dependencyAt(other, k) == stage) {
                    registry.markStale(other);
                    break;
                }
            }
        }
    }

    // Decision 3, last step of a re-run, after the stage is set Running: the prior output
    // goes. A crash part-way leaves the stage Running, which the next run treats as a re-run.
    void discardPriorOutput(Store& store) {
        if (gFailNextRerunDiscard.exchange(false)) throw IoError("injected re-run discard fault");
        store.objects().discardStage(stage);
        store.edges().discardEdgesOfStage(stage);
        store.annotations().discardAnnotationsOfStage(stage);
        store.rasters().discardRasterOfStage(stage);
    }

    // Decision 5, best effort: each step is tried whatever the one before did, and a step
    // that throws is logged naming the stage. Nothing here throws, so the exception being
    // handled stays the one that propagates.
    void recordFailure(Store& store, RasterWriterPtr& rasterWriter) noexcept {
        objects = 0;
        edges = 0;
        injectFault = gFailNextRecording.exchange(false);
        guarded("abandoning the raster writer", [&] { rasterWriter.reset(); });
        guarded("discarding its objects", [&] { store.objects().discardStage(stage); });
        guarded("discarding its edges", [&] { store.edges().discardEdgesOfStage(stage); });
        guarded("discarding its annotations", [&] { store.annotations().discardAnnotationsOfStage(stage); });
        guarded("discarding its raster", [&] { store.rasters().discardRasterOfStage(stage); });
        guarded("marking it Failed", [&] { store.stageStateWriter().setStatus(stage, StageStatus::Failed); });
        injectFault = false;
    }

    template <class Step> void guarded(const char* what, Step step) noexcept {
        try {
            if (injectFault) throw IoError("injected recording fault");
            step();
        } catch (const std::exception& e) {
            logMessage(LogLevel::Error, "StageRunner: stage \"%s\": %s failed: %s", name.c_str(), what, e.what());
        } catch (...) {
            logMessage(LogLevel::Error, "StageRunner: stage \"%s\": %s failed", name.c_str(), what);
        }
    }

    // The context of a StageError built from `original`: this stage, and the operation
    // and tile the original names.
    ErrorContext contextOf(const ProcessorError& original) const {
        ErrorContext context;
        context.stageName = name.c_str();
        if (original.operationName()[0] != '\0') context.operationName = original.operationName();
        context.cause = original.what();
        context.hasTile = original.hasTile();
        context.tileX = original.tileX();
        context.tileY = original.tileY();
        return context;
    }

    // Everything after gate(): the run proper, §4.4's listing from the re-run preparation
    // on. All of it is inside the guarded region, so any failure records Failed.
    void execute() {
        StageRegistry& registry = workspace.stages();
        const StageDefinition definition = registry.definition(stage);
        const StorePtr store = workspace.store();
        LayerRegistry& layers = workspace.layers();

        rerun = registry.status(stage) != StageStatus::Pending;
        objects = 0;
        edges = 0;
        RasterWriterPtr rasterWriter;  // outside the try: the failure path abandons it
        try {
            // Decision 3's order: dependents stale, then Running, then the discards, so a
            // crash between any two steps never leaves the stage Complete with part of its
            // output gone, nor its dependents looking current.
            if (rerun) markDependentsStale(registry);
            store->stageStateWriter().setStatus(stage, StageStatus::Running);
            if (rerun) discardPriorOutput(*store);

            const ObjectWriterPtr sink = store->objects().writerForStage(stage);
            const EdgeWriterPtr edgeWriter = store->edges().writerForStage(stage);
            const AnnotationWriterPtr notes = store->annotations().writerForStage(stage);

            // The restriction is resolved per tile by the mask source, which therefore lives
            // as long as the run; null when the stage is unrestricted.
            TileMaskSourcePtr mask;
            if (definition.hasLayerRestriction()) {
                const LayerId source = definition.layerRestriction();
                const double halfStep =
                    LevelSelector(workspace.scene()).scaleOfLevel(layers.coarsestAnalysisLevel(source)) / 2.0;
                mask = makeLayerMaskSource(store->objects(), source, halfStep);
            }

            // Auxiliary rasters (layer-backed and stored) and the raster writer, §4.4's
            // order. Built before the Pipeline, which holds raw pointers to them, so it is
            // destroyed first; they live until runInto returns.
            std::vector<TileRasterSourcePtr> sources;
            for (int32_t i = 0; i < definition.rasterLayerCount(); ++i) {
                sources.push_back(makeLayerRasterSource(store->objects(), definition.rasterLayerAt(i),
                                                        definition.rasterContentAt(i)));
            }
            for (int32_t i = 0; i < definition.readRasterCount(); ++i) {
                sources.push_back(store->rasters().sourceFor(definition.readRasterAt(i)));
            }
            if (definition.writesAnyRaster()) {
                rasterWriter = store->rasters().writerForStage(stage, definition.writtenRaster());
            }

            Pipeline pipeline(workspace.scene(), workspace.scratchConfiguration(), definition.pipeline());
            pipeline.memoryBudget(workspace.memoryBudget())
                .edgeSink(edgeWriter.get())
                .stageName(name.c_str())
                .annotationSink(notes.get())
                .maskSource(mask.get());
            for (int32_t i = 0; i < definition.writtenLayerCount(); ++i) {
                pipeline.bindLayer(layers.name(definition.writtenLayerAt(i)), definition.writtenLayerAt(i));
            }
            // The layer-backed sources come first in `sources`, then the stored ones.
            size_t next = 0;
            for (int32_t i = 0; i < definition.rasterLayerCount(); ++i) {
                pipeline.auxiliaryRaster(definition.rasterBindingAt(i), sources[next++].get());
            }
            for (int32_t i = 0; i < definition.readRasterCount(); ++i) {
                pipeline.auxiliaryRaster(definition.readRasterBindingAt(i), sources[next++].get());
            }
            if (rasterWriter) pipeline.rasterSink(rasterWriter.get());
            pipeline.validate();

            pipeline.runInto(*sink, progress, cancel);

            sink->flush();  // endpoints first
            edgeWriter->flush();  // then the edges naming them
            notes->flush();  // then the annotations naming them
            if (rasterWriter) rasterWriter->flush();  // the raster is published last
            objects = sink->emittedCount();
            edges = edgeWriter->assertedCount();

            for (int32_t i = 0; i < definition.writtenLayerCount(); ++i) {
                layers.recordAnalysisLevel(definition.writtenLayerAt(i), pipeline.analysisLevel());
            }
            store->stageStateWriter().setStatus(stage, StageStatus::Complete);
            registry.clearStale(stage);
        } catch (const CancelledError& e) {
            recordFailure(*store, rasterWriter);
            throw CancelledError(("stage \"" + name + "\" was cancelled").c_str(), contextOf(e));
        } catch (const StageError&) {
            // Raised by the runner itself or by a nested stage: already names its stage.
            recordFailure(*store, rasterWriter);
            throw;
        } catch (const ProcessorError& e) {
            recordFailure(*store, rasterWriter);
            throw StageError(("stage \"" + name + "\" failed").c_str(), contextOf(e));
        } catch (const std::bad_alloc&) {
            recordFailure(*store, rasterWriter);
            throw;
        } catch (const std::exception& e) {
            recordFailure(*store, rasterWriter);
            ErrorContext context;
            context.stageName = name.c_str();
            context.cause = e.what();
            throw StageError(("stage \"" + name + "\" failed").c_str(), context);
        } catch (...) {
            recordFailure(*store, rasterWriter);
            throw;
        }
    }

    Workspace& workspace;
    const StageId stage;
    std::string name;
    ProgressObserver* progress = nullptr;
    CancellationToken cancel;
    bool rerun = false;
    bool injectFault = false;  ///< Test seam: the current recording's steps all throw.
    int64_t objects = 0;
    int64_t edges = 0;
};

StageRunner::StageRunner(Workspace& workspace, StageId stage) : impl_(std::make_unique<Impl>(workspace, stage)) {}

StageRunner::~StageRunner() = default;

StageRunner& StageRunner::progress(ProgressObserver* observer) noexcept {
    impl_->progress = observer;
    return *this;
}

StageRunner& StageRunner::cancellation(const CancellationToken& token) noexcept {
    impl_->cancel = token;
    return *this;
}

void StageRunner::run() {
    // Read-only needs no slot; the rest of the gate runs under the slot, so a racing
    // re-run of a dependency cannot slip between the dependency check and the run.
    impl_->checkWritable();
    const detail::RunSlot slot(impl_->workspace, impl_->name.c_str());
    impl_->gate();
    impl_->execute();
}

bool StageRunner::isRerun() const noexcept { return impl_->rerun; }

int64_t StageRunner::objectsWritten() const noexcept { return impl_->objects; }

int64_t StageRunner::edgesAsserted() const noexcept { return impl_->edges; }

} // namespace slideio::processor
