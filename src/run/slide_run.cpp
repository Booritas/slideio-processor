// Requirements: UR-API-2, UR-STG-1, UR-STG-4, UR-EXE-4, UR-QA-3, UR-RES-7, UR-RES-8, UR-RES-9, UR-RES-11
#include <slideio/processor/run/slide_run.hpp>
#include <slideio/processor/run/workspace_export.hpp>
#include "core/detail/diagnostics.hpp"

#include <workspace/detail/stage_record.hpp>

#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/provenance.hpp>
#include <slideio/processor/pipeline/operation_catalog.hpp>
#include <slideio/processor/pipeline/pipeline_spec.hpp>
#include <slideio/processor/reporting/export_result.hpp>
#include <slideio/processor/storage/layer_registry.hpp>
#include <slideio/processor/storage/raster_store.hpp>
#include <slideio/processor/storage/store.hpp>
#include <slideio/processor/workspace/stage_registry.hpp>
#include <slideio/processor/workspace/stage_runner.hpp>
#include <slideio/processor/workspace/workspace.hpp>

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace slideio::processor {

namespace fs = std::filesystem;

namespace {

// The store file of a workspace directory (Workspace's documented layout); its presence is
// what Workspace::open() requires.
constexpr const char* kStoreName = "workspace.store";

const char* kindName(ExportKind kind) {
    switch (kind) {
        case ExportKind::Csv:
            return "csv";
        case ExportKind::GeoJson:
            return "geojson";
        case ExportKind::LabelMask:
            return "labelMask";
        case ExportKind::RasterTiff:
            return "rasterTiff";
    }
    return "export";
}

[[noreturn]] void refuse(const std::string& message) {
    throw InvalidArgumentError(("SlideRun: " + message).c_str());
}

void requirePath(const char* value, const char* what) {
    if (value == nullptr || value[0] == '\0') {
        refuse(std::string("the ") + what + " is null or empty");
    }
}

// The pipeline of a stage record, as PipelineSpec JSON. PipelineFile has checked that the
// record is an object whose pipeline parses.
PipelineSpec pipelineOf(const char* stageName, const char* record) {
    rapidjson::Document doc;
    doc.Parse(record);
    if (doc.HasParseError() || !doc.IsObject() || !doc.HasMember("pipeline") || !doc["pipeline"].IsObject()) {
        refuse(std::string("the record of stage '") + stageName + "' has no pipeline");
    }
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    doc["pipeline"].Accept(writer);
    return PipelineSpec::fromJson(buffer.GetString());
}

// Step 1: every operation is registered, then each builds from its parameters.
void validateOperations(const PipelineFile& file) {
    const OperationCatalog& catalog = OperationCatalog::instance();
    std::vector<PipelineSpec> pipelines;
    pipelines.reserve(static_cast<size_t>(file.stageCount()));
    for (int32_t i = 0; i < file.stageCount(); ++i) {
        pipelines.push_back(pipelineOf(file.stageName(i), file.stageRecord(i)));
        const PipelineSpec& spec = pipelines.back();
        for (int32_t j = 0; j < spec.operationCount(); ++j) {
            if (!catalog.isRegistered(spec.operationName(j))) {
                ErrorContext context;
                context.stageName = file.stageName(i);
                context.operationName = spec.operationName(j);
                throw InvalidArgumentError(("SlideRun: stage '" + std::string(file.stageName(i)) +
                                            "' uses the operation '" + spec.operationName(j) +
                                            "', which is not registered")
                                               .c_str(),
                                           context);
            }
        }
    }
    for (const PipelineSpec& spec : pipelines) {
        for (int32_t j = 0; j < spec.operationCount(); ++j) {
            catalog.create(spec.operationName(j), spec.operationParameters(j));
        }
    }
}

// True when both paths name one existing file; a missing file matches nothing.
bool sameFile(const char* a, const char* b) {
    std::error_code error;
    const bool same = fs::equivalent(fs::u8path(a), fs::u8path(b), error);
    return !error && same;
}

} // namespace

class SlideRun::Impl {
public:
    PipelineFile file;
    std::string slidePath;
    int32_t sceneIndex = 0;
    std::string workspacePath;
    std::string exportDirectory;
    Configuration configuration;
    ProgressObserver* observer = nullptr;
    CancellationToken token;
    std::string slideId; // written in place of the slide path in CSV and GeoJSON

    WorkspacePtr openOrCreate() const;
    void declare(Workspace& workspace, std::vector<StageId>& stages) const;
    void runStages(Workspace& workspace, const std::vector<StageId>& stages, SlideRunResult& result) const;
    void writeExports(Workspace& workspace, SlideRunResult& result) const;
};

// Step 2: an existing workspace's binding is read from its store, opened read-only, before
// the workspace is opened for writing. The probe opens no slide: the slide is opened once,
// by Workspace::open. A missing slide named for the run is reported as such; a recorded
// slide that is missing (moved or deleted) is not the run's slide.
WorkspacePtr SlideRun::Impl::openOrCreate() const {
    std::error_code error;
    const fs::path storePath = fs::u8path(workspacePath) / kStoreName;
    if (fs::is_regular_file(storePath, error)) {
        if (!fs::exists(fs::u8path(slidePath), error)) {
            throw IoError(detail::withPath("SlideRun: the slide cannot be found", slidePath).c_str());
        }
        StorePtr probe = Store::open(storePath.u8string().c_str(), OpenMode::ReadOnly, configuration);
        const Provenance provenance = probe->provenance();
        probe->close();
        const char* recorded = provenance.slideId();
        if (recorded == nullptr || !sameFile(recorded, slidePath.c_str()) ||
            provenance.sceneIndex() != sceneIndex) {
            refuse(detail::withPath("the workspace", workspacePath) + " belongs to another slide or scene than " +
                   detail::withPath("the slide", slidePath) + ", scene " + std::to_string(sceneIndex));
        }
        return Workspace::open(workspacePath.c_str(), configuration);
    }
    return Workspace::create(workspacePath.c_str(), slidePath.c_str(), sceneIndex, configuration);
}

// Step 3: layers and rasters, reused when the same, then every stage.
void SlideRun::Impl::declare(Workspace& workspace, std::vector<StageId>& stages) const {
    LayerRegistry& layers = workspace.layers();
    for (int32_t i = 0; i < file.layerCount(); ++i) {
        const char* name = file.layerName(i);
        const LayerKind kind = file.layerKind(i);
        LayerId parent = layers.pixelLayer();
        if (file.layerParent(i) != nullptr && !layers.find(file.layerParent(i), parent)) {
            refuse(std::string("the parent of layer '") + name + "' is not declared");
        }
        LayerId existing;
        if (layers.find(name, existing)) {
            const bool same = layers.kind(existing) == kind &&
                              (kind == LayerKind::Primitive || layers.parent(existing) == parent);
            if (!same) {
                refuse(std::string("the workspace has a layer '") + name + "' of another kind or parent");
            }
        } else if (kind == LayerKind::Semantic) {
            workspace.layerLifecycle().declareSemantic(name, parent);
        } else {
            workspace.layerLifecycle().declarePrimitive(name);
        }
    }
    RasterRegistry& rasters = workspace.store()->rasters();
    for (int32_t i = 0; i < file.rasterCount(); ++i) {
        const RasterDeclaration& declaration = file.rasterAt(i);
        RasterId existing;
        if (rasters.find(declaration.name(), existing)) {
            if (rasters.planeType(existing) != declaration.planeType() ||
                rasters.planeCount(existing) != declaration.planeCount()) {
                refuse(std::string("the workspace has a raster '") + declaration.name() +
                       "' of another type or plane count");
            }
        } else {
            rasters.declare(declaration);
        }
    }
    stages.clear();
    for (int32_t i = 0; i < file.stageCount(); ++i) {
        const StageDefinition definition = detail::stageFromRecord(file.stageRecord(i), layers, rasters);
        stages.push_back(workspace.stages().declare(definition));
    }
}

// Step 4: the first stage in file order that needs running and can, until none needs to.
void SlideRun::Impl::runStages(Workspace& workspace, const std::vector<StageId>& stages,
                               SlideRunResult& result) const {
    StageRegistry& registry = workspace.stages();
    std::vector<bool> ran(stages.size(), false);
    std::vector<int64_t> objects(stages.size(), 0);
    std::vector<int64_t> truncated(stages.size(), 0);
    for (;;) {
        size_t next = stages.size();
        size_t firstPending = stages.size();
        for (size_t i = 0; i < stages.size(); ++i) {
            const bool needed = registry.status(stages[i]) != StageStatus::Complete || registry.isStale(stages[i]);
            if (!needed) continue;
            if (firstPending == stages.size()) firstPending = i;
            if (registry.isRunnable(stages[i])) {
                next = i;
                break;
            }
        }
        if (next == stages.size()) {
            if (firstPending == stages.size()) break;
            const char* name = nullptr;
            const RunBlocker blocker = registry.runBlocker(stages[firstPending], name);
            const std::string stage = file.stageName(static_cast<int32_t>(firstPending));
            std::string why;
            switch (blocker) {
                case RunBlocker::Unreadable:
                    why = "its stored record could not be read";
                    break;
                case RunBlocker::UnresolvedDependency:
                    why = "it depends on '" + std::string(name ? name : "") + "', which no stage declares";
                    break;
                case RunBlocker::DependencyIncomplete:
                    why = "its dependency '" + std::string(name ? name : "") + "' is not complete";
                    break;
                case RunBlocker::DependencyStale:
                    why = "its dependency '" + std::string(name ? name : "") + "' is stale";
                    break;
                case RunBlocker::None:
                    why = "it is not runnable";
                    break;
            }
            ErrorContext context;
            context.stageName = stage.c_str();
            throw StageError(("SlideRun: stage '" + stage + "' cannot run: " + why).c_str(), context);
        }
        StageRunner runner(workspace, stages[next]);
        runner.progress(observer).cancellation(token);
        runner.run();
        ran[next] = true;
        objects[next] = runner.objectsWritten();
        truncated[next] = runner.truncatedObjects();
    }
    for (size_t i = 0; i < stages.size(); ++i) {
        result.addStage(file.stageName(static_cast<int32_t>(i)), ran[i], objects[i], truncated[i]);
    }
}

// Step 5: each export through its writer, which writes "<file>.partial" and renames it.
void SlideRun::Impl::writeExports(Workspace& workspace, SlideRunResult& result) const {
    const fs::path directory = fs::u8path(exportDirectory);
    std::error_code error;
    fs::create_directories(directory, error);
    if (error) {
        ErrorContext context;
        const std::string cause = error.message();
        context.cause = cause.c_str();
        const std::string message = detail::withPath("SlideRun: the export directory cannot be made", exportDirectory);
        throw IoError(message.c_str(), context);
    }
    for (int32_t i = 0; i < file.exportCount(); ++i) {
        const ExportSpec& spec = file.exportAt(i);
        if (token.isCancelled()) {
            throw CancelledError("SlideRun: the run was cancelled before the exports were complete");
        }
        const std::string path = (directory / fs::u8path(spec.file())).u8string();
        const ExportResult written = writeExport(workspace, spec, path.c_str(), slideId.c_str(), observer, token);
        if (written.wasCancelled()) {
            throw CancelledError(
                ("SlideRun: the " + std::string(kindName(spec.kind())) + " export was cancelled").c_str());
        }
        result.addExport(spec.file(), written.bytesWritten());
    }
}

SlideRun::SlideRun(const PipelineFile& file, const char* slidePath, int32_t sceneIndex, const char* workspacePath,
                   const char* exportDirectory, const Configuration& configuration) {
    requirePath(slidePath, "slide path");
    requirePath(workspacePath, "workspace path");
    requirePath(exportDirectory, "export directory");
    if (sceneIndex < 0) refuse("the scene index is negative");
    impl_ = std::make_unique<Impl>();
    impl_->file = file;
    impl_->slidePath = slidePath;
    impl_->sceneIndex = sceneIndex;
    impl_->workspacePath = workspacePath;
    impl_->exportDirectory = exportDirectory;
    impl_->configuration = configuration;
}

SlideRun::~SlideRun() = default;

SlideRun& SlideRun::progress(ProgressObserver* observer) noexcept {
    impl_->observer = observer;
    return *this;
}

SlideRun& SlideRun::slideId(const char* id) {
    impl_->slideId = id != nullptr ? id : "";
    return *this;
}

SlideRun& SlideRun::cancellation(const CancellationToken& token) noexcept {
    impl_->token = token;
    return *this;
}

SlideRunResult SlideRun::run() {
    validateOperations(impl_->file);
    WorkspacePtr workspace = impl_->openOrCreate();
    SlideRunResult result;
    try {
        std::vector<StageId> stages;
        impl_->declare(*workspace, stages);
        impl_->runStages(*workspace, stages, result);
        impl_->writeExports(*workspace, result);
    } catch (...) {
        // Best effort: the error in flight is the one to report.
        try {
            workspace->close();
        } catch (...) {
        }
        throw;
    }
    workspace->close();
    return result;
}

} // namespace slideio::processor
