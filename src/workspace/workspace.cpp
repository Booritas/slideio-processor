// Requirements: UR-STG-1, UR-STG-9, UR-EXE-2
#include "core/detail/diagnostics.hpp"
#include <slideio/processor/workspace/workspace.hpp>

#include <workspace/detail/create_fault.hpp>

#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/log.hpp>
#include <slideio/processor/core/provenance.hpp>

#include <slideio/slideio/scene.hpp>
#include <slideio/slideio/slide.hpp>
#include <slideio/slideio/slideio.hpp>

#include <atomic>
#include <exception>
#include <filesystem>
#include <mutex>
#include <string>
#include <system_error>
#include <utility>

namespace slideio::processor {

namespace fs = std::filesystem;

namespace {

constexpr const char* kStoreName = "workspace.store";
constexpr const char* kScratchName = "scratch";

void requirePath(const char* path, const char* operation, const char* what) {
    if (path == nullptr || path[0] == '\0') {
        throw InvalidArgumentError((std::string(operation) + ": the " + what + " path is null or empty").c_str());
    }
}

[[noreturn]] void throwIo(const std::string& message, const std::string& cause) {
    const std::string text = cause.empty() ? message : message + ": " + cause;
    throw IoError(text.c_str());
}

// Opens the slide through SlideIO's driver lookup. Any failure is an IoError naming the slide
// by its role and its path; SlideIO's own text, unaltered (it may name the path too), follows.
std::shared_ptr<slideio::Slide> openSlide(const std::string& slidePath, const char* operation) {
    const std::string message = detail::withPath(std::string(operation) + ": cannot open the slide", slidePath);
    std::shared_ptr<slideio::Slide> slide;
    try {
        slide = slideio::openSlide(slidePath, "");
    } catch (const std::exception& e) {
        throwIo(message, detail::diagnosticText(e));
    }
    if (!slide) {
        throwIo(message, "");
    }
    return slide;
}

int32_t sceneCount(const slideio::Slide& slide, const std::string& slidePath, const char* operation) {
    try {
        return slide.getNumScenes();
    } catch (const std::exception& e) {
        throwIo(detail::withPath(std::string(operation) + ": cannot read the scenes of the slide", slidePath),
                detail::diagnosticText(e));
    }
}

std::shared_ptr<slideio::Scene> sceneOf(const slideio::Slide& slide, const std::string& slidePath, int32_t index,
                                        const char* operation) {
    const std::string message =
        detail::withPath(std::string(operation) + ": cannot open a scene of the slide", slidePath);
    std::shared_ptr<slideio::Scene> scene;
    try {
        scene = slide.getScene(index);
    } catch (const std::exception& e) {
        throwIo(message, detail::diagnosticText(e));
    }
    if (!scene) {
        throwIo(message, "");
    }
    return scene;
}

// Leaves `scratch` an existing, empty directory.
void resetScratch(const fs::path& scratch, const char* operation) {
    std::error_code error;
    fs::remove_all(scratch, error);
    if (error) {
        throwIo(detail::withPath(std::string(operation) + ": cannot clear the scratch directory", scratch.u8string()),
                error.message());
    }
    fs::create_directories(scratch, error);
    if (error) {
        throwIo(detail::withPath(std::string(operation) + ": cannot create the scratch directory", scratch.u8string()),
                error.message());
    }
}

// The slide path as the binding stores it: absolute and lexically normalised, so the
// workspace reopens the slide whatever the working directory of a later process.
std::string absoluteSlidePath(const char* slidePath, const char* operation) {
    std::error_code error;
    const fs::path absolute = fs::absolute(fs::u8path(slidePath), error);
    if (error) {
        throwIo(detail::withPath(std::string(operation) + ": cannot make the slide path absolute", slidePath),
                error.message());
    }
    return absolute.lexically_normal().u8string();
}

// Tells whether `scene` belongs to the file `path`. A scene that cannot name its file, or a
// file that is missing, is a mismatch: this never throws.
bool sceneIsOfFile(const slideio::Scene& scene, const std::string& path) noexcept {
    try {
        const std::string sceneFile = scene.getFilePath();
        if (sceneFile.empty()) return false;
        std::error_code error;
        const bool same = fs::equivalent(fs::u8path(sceneFile), fs::u8path(path), error);
        return !error && same;
    } catch (...) {
        return false;
    }
}

// The checks the scene overloads make first. They are about no file, so they name none.
void requireScene(const std::shared_ptr<slideio::Scene>& scene, int32_t sceneIndex, const char* operation) {
    if (!scene) {
        throw InvalidArgumentError((std::string(operation) + ": the scene is null").c_str());
    }
    if (sceneIndex < 0) {
        throw InvalidArgumentError((std::string(operation) + ": the scene index is negative").c_str());
    }
}

// Refuses an occupied workspace path before anything else, so a mistyped path never touches
// another directory's files. Returns whether the path exists, as an empty directory.
bool requireFreePath(const fs::path& directory, const char* operation) {
    std::error_code error;
    const fs::file_status status = fs::status(directory, error);
    if (error && status.type() != fs::file_type::not_found) {
        throwIo(detail::withPath(std::string(operation) + ": cannot inspect the workspace directory",
                                 directory.u8string()),
                error.message());
    }
    const bool existed = fs::exists(status);
    if (existed) {
        const bool emptyDirectory = fs::is_directory(status) && fs::is_empty(directory, error) && !error;
        if (!emptyDirectory) {
            throwIo(detail::withPath(std::string(operation) + ": the workspace directory exists and is not empty",
                                     directory.u8string()),
                    "");
        }
    }
    return existed;
}

// Undoes a create that failed, removing only what that call made: with `madeStore`
// the store, its sidecars, its raster directory and scratch, which no one else can
// own, since CreateNew refuses an existing store; with `madeDirectory` the directory
// itself, and only when it is then empty, so another creator's files keep it alive.
// Nothing is removed recursively above what this call owns. A failure here is logged,
// naming the workspace directory, never thrown, so the create's own error reaches the caller.
void undoCreate(const fs::path& directory, bool madeDirectory, bool madeStore, const char* operation) noexcept {
    std::string where;  // the directory, once formed; empty if forming it failed
    try {
        where = directory.u8string();
        std::error_code error;
        auto record = [&error](const std::error_code& e) {
            if (e && !error) error = e;
        };
        if (madeStore) {
            const std::string store = (directory / kStoreName).u8string();
            // The lock file too: Store::close releases the lock but leaves its file.
            for (const char* suffix : {"", "-wal", "-shm", "-journal", "-writer.lock"}) {
                std::error_code e;
                fs::remove(fs::u8path(store + suffix), e);
                record(e);
            }
            std::error_code e;
            fs::remove_all(fs::u8path(store + "-rasters"), e);
            record(e);
            fs::remove_all(directory / kScratchName, e);
            record(e);
        }
        if (madeDirectory) {
            std::error_code e;
            if (fs::is_empty(directory, e) && !e) {
                fs::remove(directory, e);
            }
            record(e);
        }
        if (error) {
            logMessage(LogLevel::Warning, "%s: cannot remove what the failed create left (%s): %s", operation,
                       where.c_str(), error.message().c_str());
        }
    } catch (const std::exception& e) {
        logMessage(LogLevel::Warning, "%s: cannot remove what the failed create left (%s): %s", operation,
                   where.c_str(), e.what());
    }
}

#if defined(SLIDEIO_PROCESSOR_TEST_SEAMS)
std::atomic<bool> gFailNextCreate{false};

std::mutex gHookMutex;
void (*gHook)(void*) = nullptr;
void* gHookContext = nullptr;

// Runs and clears the hook set by hookNextWorkspaceCreateForTesting, if any.
void runCreateHook() {
    void (*hook)(void*) = nullptr;
    void* context = nullptr;
    {
        std::lock_guard<std::mutex> guard(gHookMutex);
        hook = std::exchange(gHook, nullptr);
        context = std::exchange(gHookContext, nullptr);
    }
    if (hook != nullptr) hook(context);
}
#endif

} // namespace

#if defined(SLIDEIO_PROCESSOR_TEST_SEAMS)
void detail::failNextWorkspaceCreateForTesting() noexcept { gFailNextCreate.store(true); }

void detail::hookNextWorkspaceCreateForTesting(void (*hook)(void*), void* context) noexcept {
    std::lock_guard<std::mutex> guard(gHookMutex);
    gHook = hook;
    gHookContext = context;
}
#endif

class Workspace::Impl {
public:
    Impl(const fs::path& directory, std::string slidePathIn, int32_t sceneIndexIn, bool readOnlyIn,
         const Configuration& configurationIn, StorePtr storeIn, std::shared_ptr<slideio::Slide> slideIn,
         std::shared_ptr<slideio::Scene> sceneIn)
        : path(directory.u8string()),
          slidePath(std::move(slidePathIn)),
          sceneIndex(sceneIndexIn),
          readOnly(readOnlyIn),
          configuration(configurationIn),
          scratchConfiguration(ConfigurationBuilder(configurationIn)
                                   .scratchDirectory((directory / kScratchName).u8string().c_str())
                                   .build()),
          budget(makeMemoryBudget(configurationIn.memoryCeilingBytes())),
          store(std::move(storeIn)),
          lifecycle(store->layers()),
          registry(*store),
          slide(std::move(slideIn)),
          scene(std::move(sceneIn)) {}

    const std::string path;
    const std::string slidePath;
    const int32_t sceneIndex;
    const bool readOnly;
    const Configuration configuration;
    const Configuration scratchConfiguration;
    const MemoryBudgetPtr budget;
    // Never reset: store() and layers() answer after close() with the closed store.
    const StorePtr store;
    LayerLifecycle lifecycle;
    StageRegistry registry;

    // Guards the slide, the scene and `closed` against close() on another thread.
    std::mutex mutex;
    std::shared_ptr<slideio::Slide> slide;
    std::shared_ptr<slideio::Scene> scene;
    bool closed = false;

    // Taken by the StageRunner inside run(): one runner at a time per workspace.
    std::atomic<bool> runSlotTaken{false};
};

Workspace::Workspace(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

namespace {

// What create() does once the slide and scene are in hand: makes the directory, the store
// with its provenance and the scratch directory, then has `make` build the workspace from
// the store. A failure from here removes what this call made, and only that, which it
// tracks rather than infers: a racing create on the same path may own everything else there.
template <typename Make>
WorkspacePtr createBound(const fs::path& directory, bool existed, const std::string& slideText, int32_t sceneIndex,
                         const Configuration& configuration, const char* kOperation, Make make) {
    std::error_code error;
#if defined(SLIDEIO_PROCESSOR_TEST_SEAMS)
    runCreateHook();
#endif
    bool madeDirectory = false;
    if (!existed) {
        // Not create_directories: the parent must exist, so a mistyped path makes nothing.
        madeDirectory = fs::create_directory(directory, error);
        if (error) {
            throwIo(detail::withPath(std::string(kOperation) + ": cannot create the workspace directory",
                                     directory.u8string()),
                    error.message());
        }
        if (!madeDirectory) {
            throwIo(detail::withPath(std::string(kOperation) +
                                         ": the workspace directory was created by someone else while this create ran",
                                     directory.u8string()),
                    "");
        }
    }
    bool madeStore = false;
    StorePtr store;
    try {
        store = Store::open((directory / kStoreName).u8string().c_str(), OpenMode::CreateNew, configuration);
        madeStore = true;
        Provenance provenance = store->provenance();
        provenance.setSlideId(slideText.c_str());
        provenance.setSceneIndex(sceneIndex);
        store->setProvenance(provenance);
#if defined(SLIDEIO_PROCESSOR_TEST_SEAMS)
        if (gFailNextCreate.exchange(false)) {
            throwIo(std::string(kOperation) + ": injected failure", "");
        }
#endif
        resetScratch(directory / kScratchName, kOperation);
        return make(std::move(store));
    } catch (...) {
        // The store's files are held open, and its writer lock held, until it is released.
        store.reset();
        undoCreate(directory, madeDirectory, madeStore, kOperation);
        throw;
    }
}

} // namespace

WorkspacePtr Workspace::create(const char* workspacePath, const char* slidePath, int32_t sceneIndex,
                               const Configuration& configuration) {
    constexpr const char* kOperation = "Workspace::create";
    requirePath(workspacePath, kOperation, "workspace");
    requirePath(slidePath, kOperation, "slide");
    const fs::path directory = fs::u8path(workspacePath);
    const std::string slideText = absoluteSlidePath(slidePath, kOperation);
    const bool existed = requireFreePath(directory, kOperation);

    // The slide and scene are checked before the directory is made, so a bad binding
    // leaves nothing behind.
    std::shared_ptr<slideio::Slide> slide = openSlide(slideText, kOperation);
    const int32_t scenes = sceneCount(*slide, slideText, kOperation);
    if (sceneIndex < 0 || sceneIndex >= scenes) {
        throw InvalidArgumentError(
            detail::withPath(std::string(kOperation) + ": the scene index is out of range for the slide", slideText)
                .c_str());
    }
    std::shared_ptr<slideio::Scene> scene = sceneOf(*slide, slideText, sceneIndex, kOperation);
    return createBound(directory, existed, slideText, sceneIndex, configuration, kOperation,
                       [&](StorePtr store) {
                           return WorkspacePtr(new Workspace(std::make_unique<Impl>(
                               directory, slideText, sceneIndex, false, configuration, std::move(store),
                               std::move(slide), std::move(scene))));
                       });
}

WorkspacePtr Workspace::create(const char* workspacePath, std::shared_ptr<slideio::Scene> scene,
                               const char* slidePath, int32_t sceneIndex, const Configuration& configuration) {
    constexpr const char* kOperation = "Workspace::create";
    requirePath(workspacePath, kOperation, "workspace");
    requirePath(slidePath, kOperation, "slide");
    requireScene(scene, sceneIndex, kOperation);
    const std::string slideText = absoluteSlidePath(slidePath, kOperation);
    if (!sceneIsOfFile(*scene, slideText)) {
        throw InvalidArgumentError(
            detail::withPath(std::string(kOperation) + ": the scene does not belong to the slide", slideText).c_str());
    }
    const fs::path directory = fs::u8path(workspacePath);
    const bool existed = requireFreePath(directory, kOperation);
    return createBound(directory, existed, slideText, sceneIndex, configuration, kOperation,
                       [&](StorePtr store) {
                           return WorkspacePtr(new Workspace(std::make_unique<Impl>(
                               directory, slideText, sceneIndex, false, configuration, std::move(store), nullptr,
                               std::move(scene))));
                       });
}

namespace {

// The part the open functions share: the store, and the slide scene its provenance names.
struct Opened {
    StorePtr store;
    std::string slidePath;
    int32_t sceneIndex = 0;
    std::shared_ptr<slideio::Slide> slide;
    std::shared_ptr<slideio::Scene> scene;
};

// A scene the caller already holds, and the index it claims for it.
struct GivenScene {
    std::shared_ptr<slideio::Scene> scene;
    int32_t sceneIndex;
};

// Refuses a given scene that is not the workspace's, from the provenance alone. A scene of
// another file is refused naming the workspace's slide by its path.
void requireGivenSceneMatches(const GivenScene& given, const Provenance& provenance, const char* operation) {
    if (!sceneIsOfFile(*given.scene, provenance.slideId())) {
        const std::string message = detail::withPath(
            std::string(operation) + ": the scene does not belong to the workspace's slide", provenance.slideId());
        throw InvalidArgumentError(message.c_str());
    }
    if (given.sceneIndex != provenance.sceneIndex()) {
        throw InvalidArgumentError((std::string(operation) + ": the scene index differs from the workspace's").c_str());
    }
}

// Opens the store and finds the scene: the given one, checked against the provenance and
// leaving the slide file unopened, or else the one the provenance names, opened through
// SlideIO. A given scene is checked through a read-only open first, so a refusal makes no
// writable open: it migrates nothing, takes no writer lock and creates no lock file. The
// check is repeated once the store is open for writing, in case it changed in between.
Opened openExisting(const fs::path& directory, OpenMode mode, const Configuration& configuration,
                    const char* operation, const GivenScene* given = nullptr) {
    const fs::path storePath = directory / kStoreName;
    std::error_code error;
    if (!fs::is_regular_file(storePath, error)) {
        throwIo(detail::withPath(std::string(operation) + ": the workspace directory holds no workspace store",
                                 directory.u8string()),
                "");
    }
    if (given != nullptr && mode != OpenMode::ReadOnly) {
        StorePtr probe = Store::open(storePath.u8string().c_str(), OpenMode::ReadOnly, configuration);
        requireGivenSceneMatches(*given, probe->provenance(), operation);
        probe->close();
    }
    Opened opened;
    opened.store = Store::open(storePath.u8string().c_str(), mode, configuration);
    const Provenance provenance = opened.store->provenance();
    opened.slidePath = provenance.slideId();
    opened.sceneIndex = provenance.sceneIndex();
    if (given != nullptr) {
        requireGivenSceneMatches(*given, provenance, operation);
        opened.scene = given->scene;
        return opened;
    }
    opened.slide = openSlide(opened.slidePath, operation);
    if (opened.sceneIndex >= sceneCount(*opened.slide, opened.slidePath, operation)) {
        throwIo(detail::withPath(std::string(operation) + ": the slide no longer has the workspace's scene",
                                 opened.slidePath),
                "");
    }
    opened.scene = sceneOf(*opened.slide, opened.slidePath, opened.sceneIndex, operation);
    return opened;
}

// The writer lock is held, so nothing live owns scratch or an unreferenced raster
// file: what is there was left by a process that died mid-stage.
void reclaimAfterCrash(const fs::path& directory, Store& store, const char* operation) {
    resetScratch(directory / kScratchName, operation);
    const int64_t reclaimed = store.rasters().removeUnreferencedFiles();
    logMessage(LogLevel::Info, "%s: reclaimed %lld bytes of unreferenced raster files (%s)", operation,
               static_cast<long long>(reclaimed), store.rasters().directory());
}

} // namespace

WorkspacePtr Workspace::open(const char* workspacePath, const Configuration& configuration) {
    constexpr const char* kOperation = "Workspace::open";
    requirePath(workspacePath, kOperation, "workspace");
    const fs::path directory = fs::u8path(workspacePath);
    Opened opened = openExisting(directory, OpenMode::ReadWrite, configuration, kOperation);
    reclaimAfterCrash(directory, *opened.store, kOperation);
    return WorkspacePtr(new Workspace(std::make_unique<Impl>(directory, std::move(opened.slidePath),
                                                             opened.sceneIndex, false, configuration,
                                                             std::move(opened.store), std::move(opened.slide),
                                                             std::move(opened.scene))));
}

WorkspacePtr Workspace::open(const char* workspacePath, std::shared_ptr<slideio::Scene> scene, int32_t sceneIndex,
                             const Configuration& configuration) {
    constexpr const char* kOperation = "Workspace::open";
    requirePath(workspacePath, kOperation, "workspace");
    requireScene(scene, sceneIndex, kOperation);
    const fs::path directory = fs::u8path(workspacePath);
    const GivenScene given{std::move(scene), sceneIndex};
    Opened opened = openExisting(directory, OpenMode::ReadWrite, configuration, kOperation, &given);
    reclaimAfterCrash(directory, *opened.store, kOperation);
    return WorkspacePtr(new Workspace(std::make_unique<Impl>(directory, std::move(opened.slidePath),
                                                             opened.sceneIndex, false, configuration,
                                                             std::move(opened.store), nullptr,
                                                             std::move(opened.scene))));
}

WorkspacePtr Workspace::openReadOnly(const char* workspacePath, const Configuration& configuration) {
    constexpr const char* kOperation = "Workspace::openReadOnly";
    requirePath(workspacePath, kOperation, "workspace");
    const fs::path directory = fs::u8path(workspacePath);
    Opened opened = openExisting(directory, OpenMode::ReadOnly, configuration, kOperation);
    return WorkspacePtr(new Workspace(std::make_unique<Impl>(directory, std::move(opened.slidePath),
                                                             opened.sceneIndex, true, configuration,
                                                             std::move(opened.store), std::move(opened.slide),
                                                             std::move(opened.scene))));
}

Workspace::~Workspace() {
    try {
        close();
    } catch (const std::exception& e) {
        logMessage(LogLevel::Error, "Workspace: closing the workspace failed (%s): %s", impl_->path.c_str(), e.what());
    } catch (...) {
        logMessage(LogLevel::Error, "Workspace: closing the workspace failed (%s)", impl_->path.c_str());
    }
}

const char* Workspace::path() const noexcept { return impl_->path.c_str(); }

const char* Workspace::slidePath() const noexcept { return impl_->slidePath.c_str(); }

int32_t Workspace::sceneIndex() const noexcept { return impl_->sceneIndex; }

bool Workspace::isReadOnly() const noexcept { return impl_->readOnly; }

StorePtr Workspace::store() noexcept { return impl_->store; }

std::shared_ptr<slideio::Scene> Workspace::scene() noexcept {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    return impl_->scene;
}

const Configuration& Workspace::configuration() const noexcept { return impl_->configuration; }

const Configuration& Workspace::scratchConfiguration() const noexcept { return impl_->scratchConfiguration; }

MemoryBudgetPtr Workspace::memoryBudget() noexcept { return impl_->budget; }

LayerRegistry& Workspace::layers() noexcept { return impl_->store->layers(); }

LayerLifecycle& Workspace::layerLifecycle() noexcept { return impl_->lifecycle; }

StageRegistry& Workspace::stages() noexcept { return impl_->registry; }

bool detail::takeRunSlot(Workspace& workspace) noexcept { return !workspace.impl_->runSlotTaken.exchange(true); }

void detail::releaseRunSlot(Workspace& workspace) noexcept { workspace.impl_->runSlotTaken.store(false); }

void Workspace::close() {
    std::shared_ptr<slideio::Scene> scene;
    std::shared_ptr<slideio::Slide> slide;
    {
        std::lock_guard<std::mutex> guard(impl_->mutex);
        if (impl_->closed) return;
        impl_->closed = true;
        scene = std::move(impl_->scene);
        slide = std::move(impl_->slide);
    }
    // The scene goes before the slide that owns it; the store last, releasing the lock.
    scene.reset();
    slide.reset();
    impl_->store->close();
}

} // namespace slideio::processor
