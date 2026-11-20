// Requirements: UR-STG-3, UR-STG-5, UR-STG-6, UR-STG-7, UR-STG-10, UR-EXE-1, UR-EXE-4, UR-EXE-5
#include <slideio/processor/workspace/stage_runner.hpp>

#include <workspace/detail/run_fault.hpp>
#include <workspace/detail/run_slot.hpp>

#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/flow.hpp>
#include <slideio/processor/core/log.hpp>
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
#include <exception>
#include <ios>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <system_error>
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
        noteStatus();
    }

    // Decision 2: whether a run now replaces an earlier one.
    void noteStatus() { rerun = workspace.stages().status(stage) != StageStatus::Pending; }

    // The first refusal; it needs no run slot.
    void checkWritable() const {
        if (workspace.isReadOnly()) {
            throw IoError(("stage \"" + name + "\" cannot run: the workspace is read-only").c_str(),
                          contextOf(nullptr));
        }
    }

    // The refusals made while the caller holds the run slot, in §4.4's order, but for the
    // last, prepare(). Each leaves the stage as it was: nothing has been written yet. An
    // error of the reads themselves (a closed workspace) is raised for this stage too.
    void gate() {
        try {
            noteStatus();  // again under the slot: no other run can change it from here
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
            checkRasterOwnership(registry.definition(stage));
        } catch (...) {
            rethrowForStage("\" cannot run");
        }
    }

    // A raster another stage populated stays that stage's until it is discarded (ruling
    // S47). RasterRegistry::writerForStage() refuses it too, but only inside the run,
    // after a re-run has discarded this stage's output; this read-only check refuses it
    // first. A populated row whose file is missing is left to writerForStage().
    void checkRasterOwnership(const StageDefinition& definition) const {
        if (!definition.writesAnyRaster()) return;
        const RasterRegistry& rasters = workspace.store()->rasters();
        const RasterId raster = definition.writtenRaster();
        if (!rasters.isPopulated(raster)) return;
        const StageId writer = rasters.writerStage(raster);
        if (writer == stage) return;
        const StageRegistry& registry = workspace.stages();
        bool known = false;
        for (int32_t i = 0; i < registry.count() && !known; ++i) known = registry.at(i) == writer;
        const std::string writerName =
            known ? "stage \"" + std::string(registry.name(writer)) + "\"" : "stage " + std::to_string(writer.value());
        throwStage(name, "stage \"" + name + "\" cannot run: its raster \"" + rasters.name(raster) +
                             "\" was written by " + writerName + ", which alone may write it until it is discarded");
    }

    // What the run reads from, built and validated at the gate. The Pipeline holds raw
    // pointers to the mask and the sources, so it is declared last and destroyed first.
    struct Prepared {
        std::optional<StageDefinition> definition;
        StorePtr store;
        TileMaskSourcePtr mask;  // null when the stage is unrestricted
        std::vector<TileRasterSourcePtr> sources;  // the layer-backed first, then the stored
        std::unique_ptr<Pipeline> pipeline;
    };

    // The last refusal (decision 6): the stage's Pipeline is built, bound to its written
    // layers, its auxiliary rasters and its mask, and validated, before anything is
    // written. Whether every object-producing operation targets a written layer is the
    // pipeline's check alone. A failure here is a refusal, raised for this stage, and the
    // stage is left as it was. Nothing built here writes: the mask and the sources read
    // per tile. The sinks are attached after the re-run preparation; validate() does not
    // consult them.
    Prepared prepare() const {
        try {
            Prepared prepared;
            prepared.definition.emplace(workspace.stages().definition(stage));
            prepared.store = workspace.store();
            const StageDefinition& definition = *prepared.definition;
            Store& store = *prepared.store;
            const LayerRegistry& layers = workspace.layers();
            if (definition.hasLayerRestriction()) {
                const LayerId source = definition.layerRestriction();
                const double halfStep =
                    LevelSelector(workspace.scene()).scaleOfLevel(layers.coarsestAnalysisLevel(source)) / 2.0;
                prepared.mask = makeLayerMaskSource(store.objects(), source, halfStep);
            }
            for (int32_t i = 0; i < definition.rasterLayerCount(); ++i) {
                prepared.sources.push_back(makeLayerRasterSource(store.objects(), definition.rasterLayerAt(i),
                                                                 definition.rasterContentAt(i)));
            }
            for (int32_t i = 0; i < definition.readRasterCount(); ++i) {
                prepared.sources.push_back(store.rasters().sourceFor(definition.readRasterAt(i)));
            }
            prepared.pipeline = std::make_unique<Pipeline>(workspace.scene(), workspace.scratchConfiguration(),
                                                           definition.pipeline());
            Pipeline& pipeline = *prepared.pipeline;
            pipeline.memoryBudget(workspace.memoryBudget()).stageName(name.c_str()).maskSource(prepared.mask.get());
            for (int32_t i = 0; i < definition.writtenLayerCount(); ++i) {
                pipeline.bindLayer(layers.name(definition.writtenLayerAt(i)), definition.writtenLayerAt(i));
            }
            size_t next = 0;
            for (int32_t i = 0; i < definition.rasterLayerCount(); ++i) {
                pipeline.auxiliaryRaster(definition.rasterBindingAt(i), prepared.sources[next++].get());
            }
            for (int32_t i = 0; i < definition.readRasterCount(); ++i) {
                pipeline.auxiliaryRaster(definition.readRasterBindingAt(i), prepared.sources[next++].get());
            }
            pipeline.validate();
            return prepared;
        } catch (...) {
            rethrowForStage("\" cannot run: preparing its pipeline failed");
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

    // The context of an error raised for this stage, with `cause` (may be null).
    ErrorContext contextOf(const char* cause) const {
        ErrorContext context;
        context.stageName = name.c_str();
        context.cause = cause;
        return context;
    }

    // The context of an error built from `original`: this stage, the original's text as
    // the cause, and the operation and tile the original names.
    ErrorContext contextOf(const ProcessorError& original) const {
        ErrorContext context = contextOf(original.what());
        if (original.operationName()[0] != '\0') context.operationName = original.operationName();
        context.hasTile = original.hasTile();
        context.tileX = original.tileX();
        context.tileY = original.tileY();
        return context;
    }

    // Decision 5 and error.hpp's exception stance, applied to the exception being
    // handled; call only from a handler. `outcome` completes "stage \"<name>" into the
    // message. A StageError naming this stage passes through unchanged; one naming
    // another stage (a nested runner's refusal) is wrapped like any ProcessorError.
    [[noreturn]] void rethrowForStage(const char* outcome) const {
        const std::string message = "stage \"" + name + outcome;
        try {
            throw;
        } catch (const CancelledError& e) {
            throw CancelledError(("stage \"" + name + "\" was cancelled").c_str(), contextOf(e));
        } catch (const StageError& e) {
            if (name == e.stageName()) throw;
            throw StageError(message.c_str(), contextOf(e));
        } catch (const ProcessorError& e) {
            throw StageError(message.c_str(), contextOf(e));
        } catch (const std::bad_alloc&) {
            throw;
        } catch (const std::ios_base::failure& e) {
            throw IoError(message.c_str(), contextOf(e.what()));
        } catch (const std::system_error& e) {  // std::filesystem::filesystem_error too
            throw IoError(message.c_str(), contextOf(e.what()));
        } catch (const std::exception& e) {
            throw InternalError(message.c_str(), contextOf(e.what()));
        } catch (...) {
            throw InternalError((message + " with an exception of unknown type").c_str(), contextOf(nullptr));
        }
    }

    // A stage may read the raster it writes (readsRaster and writesRaster naming one
    // raster). prepare() opened that raster's prior file, which a re-run must neither read
    // (UR-STG-6: the re-run would see its own earlier output) nor hold open while the
    // discard deletes it (Windows). So on a re-run each such source is unbound and
    // dropped before the discards, and re-acquired after them, reading the raster as it
    // now is, unpopulated. Rebinding resets the plan; runInto() validates again.
    template <class Each> void forEachSelfRead(const Prepared& prepared, Each each) const {
        const StageDefinition& definition = *prepared.definition;
        if (!definition.writesAnyRaster()) return;
        const size_t base = static_cast<size_t>(definition.rasterLayerCount());
        for (int32_t i = 0; i < definition.readRasterCount(); ++i) {
            if (definition.readRasterAt(i) == definition.writtenRaster()) {
                each(definition.readRasterBindingAt(i), base + static_cast<size_t>(i));
            }
        }
    }

    void releaseSelfReads(Prepared& prepared) const {
        forEachSelfRead(prepared, [&prepared](const char* binding, size_t index) {
            prepared.pipeline->auxiliaryRaster(binding, nullptr);
            prepared.sources[index].reset();
        });
    }

    void rebindSelfReads(Prepared& prepared) const {
        forEachSelfRead(prepared, [&prepared](const char* binding, size_t index) {
            prepared.sources[index] = prepared.store->rasters().sourceFor(prepared.definition->writtenRaster());
            prepared.pipeline->auxiliaryRaster(binding, prepared.sources[index].get());
        });
    }

    // Everything after prepare(): the run proper, §4.4's listing from the re-run
    // preparation on. All of it is inside the guarded region, so any failure records Failed.
    void execute(Prepared& prepared) {
        StageRegistry& registry = workspace.stages();
        const StageDefinition& definition = *prepared.definition;
        const StorePtr& store = prepared.store;
        LayerRegistry& layers = workspace.layers();
        Pipeline& pipeline = *prepared.pipeline;

        objects = 0;
        edges = 0;
        RasterWriterPtr rasterWriter;  // outside the try: the failure path abandons it
        try {
            // Decision 3's order: dependents stale, then Running, then the discards, so a
            // crash between any two steps never leaves the stage Complete with part of its
            // output gone, nor its dependents looking current. Step 1 marks every stage the
            // staleness graph reaches from this one (SDS 04 4.5), whatever this run's outcome.
            if (rerun) registry.markAffectedStale(stage);
            store->stageStateWriter().setStatus(stage, StageStatus::Running);
            if (rerun) {
                releaseSelfReads(prepared);  // first: an open source would keep the file
                discardPriorOutput(*store);
                rebindSelfReads(prepared);
            }

            // The object, edge and annotation writers write nothing until given something.
            // The raster writer creates the raster's directory, so it is obtained here,
            // after the discards, and not at the gate.
            const ObjectWriterPtr sink = store->objects().writerForStage(stage);
            const EdgeWriterPtr edgeWriter = store->edges().writerForStage(stage);
            const AnnotationWriterPtr notes = store->annotations().writerForStage(stage);
            if (definition.writesAnyRaster()) {
                rasterWriter = store->rasters().writerForStage(stage, definition.writtenRaster());
            }
            pipeline.edgeSink(edgeWriter.get()).annotationSink(notes.get());
            if (rasterWriter) pipeline.rasterSink(rasterWriter.get());

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
        } catch (...) {
            recordFailure(*store, rasterWriter);
            rethrowForStage("\" failed");
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
    // Decision 2, decided first so that a refused run answers for its own start too.
    try {
        impl_->noteStatus();
    } catch (...) {
        impl_->rethrowForStage("\" cannot run");
    }
    // Read-only needs no slot; the rest of the gate runs under the slot, so a racing
    // re-run of a dependency cannot slip between the dependency check and the run.
    impl_->checkWritable();
    const detail::RunSlot slot(impl_->workspace, impl_->name.c_str());
    impl_->gate();
    Impl::Prepared prepared = impl_->prepare();
    impl_->execute(prepared);
}

bool StageRunner::isRerun() const noexcept { return impl_->rerun; }

int64_t StageRunner::objectsWritten() const noexcept { return impl_->objects; }

int64_t StageRunner::edgesAsserted() const noexcept { return impl_->edges; }

} // namespace slideio::processor
