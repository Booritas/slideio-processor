// Requirements: UR-STG-10, UR-EXE-1, UR-EXE-4
#include <slideio/processor/workspace/stage_runner.hpp>

#include <workspace/detail/run_slot.hpp>

#include <slideio/processor/core/error.hpp>
#include <slideio/processor/pipeline/pipeline.hpp>
#include <slideio/processor/storage/annotation_store.hpp>
#include <slideio/processor/storage/edge_store.hpp>
#include <slideio/processor/storage/layer_registry.hpp>
#include <slideio/processor/storage/object_store.hpp>
#include <slideio/processor/storage/stage_state_reader.hpp>
#include <slideio/processor/storage/stage_state_writer.hpp>
#include <slideio/processor/storage/store.hpp>
#include <slideio/processor/workspace/stage_definition.hpp>
#include <slideio/processor/workspace/stage_registry.hpp>

#include <string>

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

} // namespace

class StageRunner::Impl {
public:
    Impl(Workspace& workspaceIn, StageId stageIn) : workspace(workspaceIn), stage(stageIn) {
        StageRegistry& registry = workspace.stages();
        registry.isUnreadable(stage);  // throws InvalidArgumentError for an unknown stage
        name = registry.name(stage);
        rerun = registry.status(stage) != StageStatus::Pending;
    }

    // The refusals that leave the stage as it was, in §4.4's order. The run slot is the
    // last of them and is taken by the caller.
    void gate() const {
        if (workspace.isReadOnly()) {
            throw IoError(("stage \"" + name + "\" cannot run: the workspace is read-only").c_str());
        }
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
    }

    // Everything after the run slot is taken: the run proper, §4.4's listing without the
    // layer restriction and the rasters.
    void execute() {
        StageRegistry& registry = workspace.stages();
        const StageDefinition definition = registry.definition(stage);
        const StorePtr store = workspace.store();
        LayerRegistry& layers = workspace.layers();

        rerun = registry.status(stage) != StageStatus::Pending;
        objects = 0;
        edges = 0;
        store->stageStateWriter().setStatus(stage, StageStatus::Running);
        try {
            const ObjectWriterPtr sink = store->objects().writerForStage(stage);
            const EdgeWriterPtr edgeWriter = store->edges().writerForStage(stage);
            const AnnotationWriterPtr notes = store->annotations().writerForStage(stage);

            Pipeline pipeline(workspace.scene(), workspace.scratchConfiguration(), definition.pipeline());
            pipeline.memoryBudget(workspace.memoryBudget())
                .edgeSink(edgeWriter.get())
                .stageName(name.c_str())
                .annotationSink(notes.get());
            for (int32_t i = 0; i < definition.writtenLayerCount(); ++i) {
                pipeline.bindLayer(layers.name(definition.writtenLayerAt(i)), definition.writtenLayerAt(i));
            }
            pipeline.validate();

            pipeline.runInto(*sink, progress, cancel);

            sink->flush();  // endpoints first
            edgeWriter->flush();  // then the edges naming them
            notes->flush();  // then the annotations naming them
            objects = sink->emittedCount();
            edges = edgeWriter->assertedCount();

            for (int32_t i = 0; i < definition.writtenLayerCount(); ++i) {
                layers.recordAnalysisLevel(definition.writtenLayerAt(i), pipeline.analysisLevel());
            }
            store->stageStateWriter().setStatus(stage, StageStatus::Complete);
            registry.clearStale(stage);
        } catch (...) {
            objects = 0;
            edges = 0;
            // Failure handling proper (discards, wrapping) is the next step; for now the
            // stage is only marked and the exception propagates.
            store->stageStateWriter().setStatus(stage, StageStatus::Failed);
            throw;
        }
    }

    Workspace& workspace;
    const StageId stage;
    std::string name;
    ProgressObserver* progress = nullptr;
    CancellationToken cancel;
    bool rerun = false;
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
    impl_->gate();
    const detail::RunSlot slot(impl_->workspace, impl_->name.c_str());
    impl_->execute();
}

bool StageRunner::isRerun() const noexcept { return impl_->rerun; }

int64_t StageRunner::objectsWritten() const noexcept { return impl_->objects; }

int64_t StageRunner::edgesAsserted() const noexcept { return impl_->edges; }

} // namespace slideio::processor
