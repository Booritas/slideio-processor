// Requirements: UR-STG-1, UR-STG-9, UR-EXE-2
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

// Opens the slide through SlideIO's driver lookup. Any failure is an IoError naming
// the slide path, so the caller learns which file to restore or rebind.
std::shared_ptr<slideio::Slide> openSlide(const std::string& slidePath, const char* operation) {
    std::shared_ptr<slideio::Slide> slide;
    try {
        slide = slideio::openSlide(slidePath, "");
    } catch (const std::exception& e) {
        throwIo(std::string(operation) + ": cannot open the slide '" + slidePath + "'", e.what());
    }
    if (!slide) {
        throwIo(std::string(operation) + ": cannot open the slide '" + slidePath + "'", "");
    }
    return slide;
}

int32_t sceneCount(const slideio::Slide& slide, const std::string& slidePath, const char* operation) {
    try {
        return slide.getNumScenes();
    } catch (const std::exception& e) {
        throwIo(std::string(operation) + ": cannot read the scenes of the slide '" + slidePath + "'", e.what());
    }
}

std::shared_ptr<slideio::Scene> sceneOf(const slideio::Slide& slide, int32_t index, const std::string& slidePath,
                                        const char* operation) {
    std::shared_ptr<slideio::Scene> scene;
    try {
        scene = slide.getScene(index);
    } catch (const std::exception& e) {
        throwIo(std::string(operation) + ": cannot open a scene of the slide '" + slidePath + "'", e.what());
    }
    if (!scene) {
        throwIo(std::string(operation) + ": cannot open a scene of the slide '" + slidePath + "'", "");
    }
    return scene;
}

// Leaves `scratch` an existing, empty directory.
void resetScratch(const fs::path& scratch, const char* operation) {
    std::error_code error;
    fs::remove_all(scratch, error);
    if (error) {
        throwIo(std::string(operation) + ": cannot clear the scratch directory '" + scratch.u8string() + "'",
                error.message());
    }
    fs::create_directories(scratch, error);
    if (error) {
        throwIo(std::string(operation) + ": cannot create the scratch directory '" + scratch.u8string() + "'",
                error.message());
    }
}

// The slide path as the binding stores it: absolute and lexically normalised, so the
// workspace reopens the slide whatever the working directory of a later process.
std::string absoluteSlidePath(const char* slidePath, const char* operation) {
    std::error_code error;
    const fs::path absolute = fs::absolute(fs::u8path(slidePath), error);
    if (error) {
        throwIo(std::string(operation) + ": cannot make the slide path '" + slidePath + "' absolute",
                error.message());
    }
    return absolute.lexically_normal().u8string();
}

// Undoes a create that failed, removing only what that call made: with `madeStore`
// the store, its sidecars, its raster directory and scratch, which no one else can
// own, since CreateNew refuses an existing store; with `madeDirectory` the directory
// itself, and only when it is then empty, so another creator's files keep it alive.
// Nothing is removed recursively above what this call owns. A failure here is logged,
// never thrown, so the create's own error reaches the caller.
void undoCreate(const fs::path& directory, bool madeDirectory, bool madeStore, const char* operation) noexcept {
    try {
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
            logMessage(LogLevel::Warning, "%s: cannot remove what the failed create left in '%s': %s", operation,
                       directory.u8string().c_str(), error.message().c_str());
        }
    } catch (const std::exception& e) {
        logMessage(LogLevel::Warning, "%s: cannot remove what the failed create left: %s", operation, e.what());
    }
}

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

} // namespace

void detail::failNextWorkspaceCreateForTesting() noexcept { gFailNextCreate.store(true); }

void detail::hookNextWorkspaceCreateForTesting(void (*hook)(void*), void* context) noexcept {
    std::lock_guard<std::mutex> guard(gHookMutex);
    gHook = hook;
    gHookContext = context;
}

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

    // Guards the slide, the scene and `closed` against close() on another thread.
    std::mutex mutex;
    std::shared_ptr<slideio::Slide> slide;
    std::shared_ptr<slideio::Scene> scene;
    bool closed = false;
};

Workspace::Workspace(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

WorkspacePtr Workspace::create(const char* workspacePath, const char* slidePath, int32_t sceneIndex,
                               const Configuration& configuration) {
    constexpr const char* kOperation = "Workspace::create";
    requirePath(workspacePath, kOperation, "workspace");
    requirePath(slidePath, kOperation, "slide");
    const fs::path directory = fs::u8path(workspacePath);
    const std::string slideText = absoluteSlidePath(slidePath, kOperation);

    // Refuse an occupied path before anything else, so a mistyped path never touches
    // another directory's files.
    std::error_code error;
    const fs::file_status status = fs::status(directory, error);
    if (error && status.type() != fs::file_type::not_found) {
        throwIo(std::string(kOperation) + ": cannot inspect '" + directory.u8string() + "'", error.message());
    }
    const bool existed = fs::exists(status);
    if (existed) {
        const bool emptyDirectory = fs::is_directory(status) && fs::is_empty(directory, error) && !error;
        if (!emptyDirectory) {
            throwIo(std::string(kOperation) + ": '" + directory.u8string() + "' exists and is not an empty directory",
                    "");
        }
    }

    // The slide and scene are checked before the directory is made, so a bad binding
    // leaves nothing behind.
    std::shared_ptr<slideio::Slide> slide = openSlide(slideText, kOperation);
    const int32_t scenes = sceneCount(*slide, slideText, kOperation);
    if (sceneIndex < 0 || sceneIndex >= scenes) {
        throw InvalidArgumentError((std::string(kOperation) + ": the scene index is out of range for the slide '" +
                                    slideText + "'")
                                       .c_str());
    }
    std::shared_ptr<slideio::Scene> scene = sceneOf(*slide, sceneIndex, slideText, kOperation);

    // From here a failure removes what this call made, and only that, so the path is
    // as it was and a retry can succeed. What it made is tracked, never inferred: a
    // racing create on the same path may own everything else there.
    runCreateHook();
    bool madeDirectory = false;
    if (!existed) {
        // Not create_directories: the parent must exist, so a mistyped path makes nothing.
        madeDirectory = fs::create_directory(directory, error);
        if (error) {
            throwIo(std::string(kOperation) + ": cannot create the workspace directory '" + directory.u8string() + "'",
                    error.message());
        }
        if (!madeDirectory) {
            throwIo(std::string(kOperation) + ": '" + directory.u8string() +
                        "' was created by someone else while this create ran",
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
        if (gFailNextCreate.exchange(false)) {
            throwIo(std::string(kOperation) + ": injected failure for '" + directory.u8string() + "'", "");
        }
        resetScratch(directory / kScratchName, kOperation);
        return WorkspacePtr(new Workspace(std::make_unique<Impl>(directory, slideText, sceneIndex, false,
                                                                 configuration, std::move(store), std::move(slide),
                                                                 std::move(scene))));
    } catch (...) {
        // The store's files are held open, and its writer lock held, until it is released.
        store.reset();
        undoCreate(directory, madeDirectory, madeStore, kOperation);
        throw;
    }
}

namespace {

// The part open() and openReadOnly() share: the store, and the slide its provenance names.
struct Opened {
    StorePtr store;
    std::string slidePath;
    int32_t sceneIndex = 0;
    std::shared_ptr<slideio::Slide> slide;
    std::shared_ptr<slideio::Scene> scene;
};

Opened openExisting(const fs::path& directory, OpenMode mode, const Configuration& configuration,
                    const char* operation) {
    const fs::path storePath = directory / kStoreName;
    std::error_code error;
    if (!fs::is_regular_file(storePath, error)) {
        throwIo(std::string(operation) + ": '" + directory.u8string() + "' holds no workspace store", "");
    }
    Opened opened;
    opened.store = Store::open(storePath.u8string().c_str(), mode, configuration);
    const Provenance provenance = opened.store->provenance();
    opened.slidePath = provenance.slideId();
    opened.sceneIndex = provenance.sceneIndex();
    opened.slide = openSlide(opened.slidePath, operation);
    if (opened.sceneIndex >= sceneCount(*opened.slide, opened.slidePath, operation)) {
        throwIo(std::string(operation) + ": the slide '" + opened.slidePath + "' no longer has the workspace's scene",
                "");
    }
    opened.scene = sceneOf(*opened.slide, opened.sceneIndex, opened.slidePath, operation);
    return opened;
}

} // namespace

WorkspacePtr Workspace::open(const char* workspacePath, const Configuration& configuration) {
    constexpr const char* kOperation = "Workspace::open";
    requirePath(workspacePath, kOperation, "workspace");
    const fs::path directory = fs::u8path(workspacePath);
    Opened opened = openExisting(directory, OpenMode::ReadWrite, configuration, kOperation);

    // The writer lock is held, so nothing live owns scratch or an unreferenced raster
    // file: what is there was left by a process that died mid-stage.
    resetScratch(directory / kScratchName, kOperation);
    const int64_t reclaimed = opened.store->rasters().removeUnreferencedFiles();
    logMessage(LogLevel::Info, "%s: reclaimed %lld bytes of unreferenced raster files in '%s'", kOperation,
               static_cast<long long>(reclaimed), opened.store->rasters().directory());

    return WorkspacePtr(new Workspace(std::make_unique<Impl>(directory, std::move(opened.slidePath),
                                                             opened.sceneIndex, false, configuration,
                                                             std::move(opened.store), std::move(opened.slide),
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
        logMessage(LogLevel::Error, "Workspace: closing '%s' failed: %s", path(), e.what());
    } catch (...) {
        logMessage(LogLevel::Error, "Workspace: closing '%s' failed", path());
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
