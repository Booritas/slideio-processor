// Requirements: UR-STG-1, UR-STG-9, UR-EXE-2
#include "common/temp_path.hpp"
#include "workspace/support/test_slide.hpp"

#include <workspace/detail/create_fault.hpp>

#include <slideio/processor/config/configuration.hpp>
#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/memory_budget.hpp>
#include <slideio/processor/storage/store.hpp>
#include <slideio/processor/workspace/workspace.hpp>

#include <slideio/slideio/scene.hpp>
#include <slideio/slideio/slide.hpp>
#include <slideio/slideio/slideio.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <tuple>

using namespace slideio::processor;
using testing_support::TempDirectory;
using testing_support::writeTestSlide;

namespace fs = std::filesystem;

// The test slide is a plain tiled TIFF; SlideIO picks its driver (GDAL) from the extension.
TEST(Workspace, TestSlideOpensThroughSlideIO) {
    TempDirectory dir;
    fs::create_directories(dir.path());
    const fs::path file = writeTestSlide(dir.path() / "slide.tif", 300, 200);
    auto slide = slideio::openSlide(file.u8string(), "");
    ASSERT_TRUE(slide);
    ASSERT_EQ(slide->getNumScenes(), 1);
    auto scene = slide->getScene(0);
    const auto rect = scene->getRect();
    EXPECT_EQ(std::get<2>(rect), 300);
    EXPECT_EQ(std::get<3>(rect), 200);
    EXPECT_EQ(scene->getNumChannels(), 3);
}

namespace {

// A 300 x 200 slide and a workspace path beside it, both in a fresh temporary
// directory. A plain struct rather than a fixture, so the tests keep the suite name
// Workspace, which a fixture class would clash with.
struct Prepared {
    Prepared() {
        fs::create_directories(dir.path());
        slide = writeTestSlide(dir.path() / "slide.tif", 300, 200);
        workspace = dir.path() / "ws";
    }

    std::string slideUtf8() const { return slide.u8string(); }
    std::string workspaceUtf8() const { return workspace.u8string(); }

    WorkspacePtr createWorkspace() const {
        return Workspace::create(workspaceUtf8().c_str(), slideUtf8().c_str(), 0, config);
    }

    TempDirectory dir;
    fs::path slide;
    fs::path workspace;
    Configuration config = Configuration::defaults();
};

int sceneWidth(const WorkspacePtr& ws) { return std::get<2>(ws->scene()->getRect()); }
int sceneHeight(const WorkspacePtr& ws) { return std::get<3>(ws->scene()->getRect()); }

} // namespace

TEST(Workspace, CreateBindsSlideAndScene) {
    const Prepared t;
    WorkspacePtr ws = t.createWorkspace();
    ASSERT_TRUE(ws);
    EXPECT_EQ(std::string(ws->path()), t.workspaceUtf8());
    EXPECT_EQ(std::string(ws->slidePath()), t.slideUtf8());
    EXPECT_EQ(ws->sceneIndex(), 0);
    EXPECT_FALSE(ws->isReadOnly());
    ASSERT_TRUE(ws->scene());
    EXPECT_EQ(sceneWidth(ws), 300);
    EXPECT_EQ(sceneHeight(ws), 200);
    EXPECT_EQ(std::string(ws->store()->provenance().slideId()), t.slideUtf8());
    EXPECT_EQ(ws->store()->provenance().sceneIndex(), 0);
    EXPECT_TRUE(fs::is_regular_file(t.workspace / "workspace.store"));
}

TEST(Workspace, CreateRefusesNonEmptyDirectory) {
    const Prepared t;
    fs::create_directories(t.workspace);
    writeFile(t.workspace / "something.txt", "x");
    EXPECT_THROW(t.createWorkspace(), IoError);
    // A plain file at the path is refused the same way.
    const fs::path file = t.dir.path() / "file";
    writeFile(file, "x");
    EXPECT_THROW(Workspace::create(file.u8string().c_str(), t.slideUtf8().c_str(), 0, t.config), IoError);
}

TEST(Workspace, CreateAcceptsEmptyDirectory) {
    const Prepared t;
    fs::create_directories(t.workspace);
    WorkspacePtr ws = t.createWorkspace();
    EXPECT_TRUE(fs::is_regular_file(t.workspace / "workspace.store"));
}

TEST(Workspace, CreateRejectsSceneIndexOutOfRange) {
    const Prepared t;
    EXPECT_THROW(Workspace::create(t.workspaceUtf8().c_str(), t.slideUtf8().c_str(), 1, t.config), InvalidArgumentError);
    EXPECT_THROW(Workspace::create(t.workspaceUtf8().c_str(), t.slideUtf8().c_str(), -1, t.config), InvalidArgumentError);
    // The slide is checked before anything is made.
    EXPECT_FALSE(fs::exists(t.workspace));
}

TEST(Workspace, OpenRestoresBinding) {
    const Prepared t;
    {
        WorkspacePtr ws = t.createWorkspace();
        ws->layers().declareSemantic("tissue", ws->layers().pixelLayer());
        ws->close();
    }
    WorkspacePtr ws = Workspace::open(t.workspaceUtf8().c_str(), t.config);
    EXPECT_EQ(std::string(ws->slidePath()), t.slideUtf8());
    EXPECT_EQ(ws->sceneIndex(), 0);
    EXPECT_FALSE(ws->isReadOnly());
    ASSERT_TRUE(ws->scene());
    EXPECT_EQ(sceneWidth(ws), 300);
    EXPECT_EQ(sceneHeight(ws), 200);
    LayerId tissue;
    EXPECT_TRUE(ws->layers().find("tissue", tissue));
}

TEST(Workspace, OpenWithMissingSlideIsIoError) {
    const Prepared t;
    t.createWorkspace()->close();
    fs::remove(t.slide);
    try {
        Workspace::open(t.workspaceUtf8().c_str(), t.config);
        FAIL() << "open succeeded without its slide";
    } catch (const IoError& e) {
        EXPECT_NE(std::string(e.what()).find(t.slideUtf8()), std::string::npos) << e.what();
    }
}

TEST(Workspace, SecondWriterRefusedReaderAllowed) {
    const Prepared t;
    t.createWorkspace()->close();
    WorkspacePtr writer = Workspace::open(t.workspaceUtf8().c_str(), t.config);
    EXPECT_THROW(Workspace::open(t.workspaceUtf8().c_str(), t.config), IoError);
    WorkspacePtr reader = Workspace::openReadOnly(t.workspaceUtf8().c_str(), t.config);
    EXPECT_TRUE(reader->isReadOnly());
    EXPECT_TRUE(reader->store()->isReadOnly());
    EXPECT_EQ(std::string(reader->slidePath()), t.slideUtf8());
    ASSERT_TRUE(reader->scene());
    EXPECT_EQ(sceneWidth(reader), 300);
}

TEST(Workspace, ScratchIsInsideAndClearedAtOpen) {
    const Prepared t;
    const fs::path scratch = t.workspace / "scratch";
    {
        WorkspacePtr ws = t.createWorkspace();
        EXPECT_EQ(std::string(ws->scratchConfiguration().scratchDirectory()), scratch.u8string());
        const Configuration expected =
            ConfigurationBuilder(ws->configuration()).scratchDirectory(scratch.u8string().c_str()).build();
        EXPECT_TRUE(ws->scratchConfiguration().equals(expected));
        EXPECT_TRUE(ws->configuration().equals(t.config));
        EXPECT_TRUE(fs::is_directory(scratch));
        ws->close();
    }
    const fs::path leftover = scratch / "leftover.spill";
    writeFile(leftover, "left by a crashed run");
    // A reader leaves scratch alone: it holds no writer lock, so a live writer may own it.
    Workspace::openReadOnly(t.workspaceUtf8().c_str(), t.config)->close();
    EXPECT_TRUE(fs::exists(leftover));
    WorkspacePtr ws = Workspace::open(t.workspaceUtf8().c_str(), t.config);
    EXPECT_FALSE(fs::exists(leftover));
    EXPECT_TRUE(fs::is_directory(scratch));
    EXPECT_EQ(std::string(ws->scratchConfiguration().scratchDirectory()), scratch.u8string());
}

TEST(Workspace, OpenSweepsUnreferencedRasterFiles) {
    const Prepared t;
    fs::path rasters;
    {
        WorkspacePtr ws = t.createWorkspace();
        rasters = fs::u8path(ws->store()->rasters().directory());
        ws->close();
    }
    EXPECT_EQ(rasters, t.workspace / "workspace.store-rasters");
    fs::create_directories(rasters);
    // 32 lowercase hex digits then ".tif": a name the library generates, which no raster names.
    const fs::path orphan = rasters / "0123456789abcdef0123456789abcdef.tif";
    const fs::path foreign = rasters / "notes.txt";
    writeFile(orphan, "orphaned raster");
    writeFile(foreign, "not the library's");
    WorkspacePtr ws = Workspace::open(t.workspaceUtf8().c_str(), t.config);
    EXPECT_FALSE(fs::exists(orphan));
    EXPECT_TRUE(fs::exists(foreign));
}

TEST(Workspace, MemoryBudgetIsOneAndSized) {
    const Prepared t;
    const Configuration config = ConfigurationBuilder(t.config).memoryCeilingBytes(int64_t(3) << 30).build();
    WorkspacePtr ws = Workspace::create(t.workspaceUtf8().c_str(), t.slideUtf8().c_str(), 0, config);
    MemoryBudgetPtr first = ws->memoryBudget();
    MemoryBudgetPtr second = ws->memoryBudget();
    ASSERT_TRUE(first);
    EXPECT_EQ(first, second);
    EXPECT_EQ(first->ceilingBytes(), ws->configuration().memoryCeilingBytes());
    EXPECT_EQ(first->ceilingBytes(), int64_t(3) << 30);
}

TEST(Workspace, NonAsciiPathsRoundTrip) {
    TempDirectory dir;
    const fs::path root = dir.path() / fs::u8path(u8"wörk_空間");
    fs::create_directories(root);
    const fs::path slide = writeTestSlide(root / fs::u8path(u8"slide_ä.tif"), 128, 96);
    const fs::path wsPath = root / fs::u8path(u8"ws_ü");
    const Configuration config = Configuration::defaults();
    {
        WorkspacePtr ws = Workspace::create(wsPath.u8string().c_str(), slide.u8string().c_str(), 0, config);
        EXPECT_EQ(std::string(ws->path()), wsPath.u8string());
        ws->close();
    }
    EXPECT_TRUE(fs::is_regular_file(wsPath / "workspace.store"));
    EXPECT_TRUE(fs::is_directory(wsPath / "scratch"));
    WorkspacePtr ws = Workspace::open(wsPath.u8string().c_str(), config);
    EXPECT_EQ(std::string(ws->slidePath()), slide.u8string());
    EXPECT_EQ(std::string(ws->store()->provenance().slideId()), slide.u8string());
    ASSERT_TRUE(ws->scene());
    EXPECT_EQ(sceneWidth(ws), 128);
    EXPECT_EQ(sceneHeight(ws), 96);
}

TEST(Workspace, CloseIsIdempotent) {
    const Prepared t;
    WorkspacePtr ws = t.createWorkspace();
    ws->close();
    EXPECT_NO_THROW(ws->close());
    // Identity still answers after close; the scene is released and the store refuses use.
    EXPECT_EQ(std::string(ws->path()), t.workspaceUtf8());
    EXPECT_EQ(std::string(ws->slidePath()), t.slideUtf8());
    EXPECT_FALSE(ws->scene());
    EXPECT_THROW(ws->store()->provenance(), IoError);
    // The writer lock is released, so the workspace opens again.
    EXPECT_NO_THROW(Workspace::open(t.workspaceUtf8().c_str(), t.config)->close());
}

// Store creation cannot be failed by any argument, since every argument is checked
// before the directory is made, so the engine's test seam fails create once its
// store exists: the latest point, with the most to undo.
TEST(Workspace, FailedCreateLeavesNoDirectory) {
    const Prepared t;
    // The path did not exist: it does not exist afterwards.
    detail::failNextWorkspaceCreateForTesting();
    EXPECT_THROW(t.createWorkspace(), IoError);
    EXPECT_FALSE(fs::exists(t.workspace));
    EXPECT_NO_THROW(t.createWorkspace()->close());

    // The path was an empty directory: it is an empty directory afterwards.
    const fs::path empty = t.dir.path() / "empty";
    fs::create_directories(empty);
    detail::failNextWorkspaceCreateForTesting();
    EXPECT_THROW(Workspace::create(empty.u8string().c_str(), t.slideUtf8().c_str(), 0, t.config), IoError);
    EXPECT_TRUE(fs::is_directory(empty));
    EXPECT_TRUE(fs::is_empty(empty));
    WorkspacePtr ws = Workspace::create(empty.u8string().c_str(), t.slideUtf8().c_str(), 0, t.config);
    EXPECT_TRUE(fs::is_regular_file(empty / "workspace.store"));
}

namespace {

// What a racing create made: the winner's workspace, created on the loser's path
// between the loser's occupancy check and its first write.
struct Race {
    const char* workspacePath = nullptr;
    const char* slidePath = nullptr;
    const Configuration* config = nullptr;
    WorkspacePtr winner;
};

void createWinner(void* context) {
    Race& race = *static_cast<Race*>(context);
    race.winner = Workspace::create(race.workspacePath, race.slidePath, 0, *race.config);
}

// The loser's create fails, and the winner's live workspace keeps every file and stays
// open and writable.
void expectLoserRemovesNothing(const fs::path& path, const std::string& slide, const Configuration& config) {
    const std::string pathText = path.u8string();
    Race race{pathText.c_str(), slide.c_str(), &config, nullptr};
    detail::hookNextWorkspaceCreateForTesting(&createWinner, &race);
    EXPECT_THROW(Workspace::create(pathText.c_str(), slide.c_str(), 0, config), IoError);
    ASSERT_TRUE(race.winner);
    EXPECT_TRUE(fs::is_regular_file(path / "workspace.store"));
    EXPECT_TRUE(fs::is_regular_file(path / "workspace.store-writer.lock"));
    EXPECT_TRUE(fs::is_directory(path / "scratch"));
    EXPECT_EQ(std::string(race.winner->store()->provenance().slideId()), slide);
    LayerId tissue;
    EXPECT_NO_THROW(tissue = race.winner->layers().declareSemantic("tissue", race.winner->layers().pixelLayer()));
    EXPECT_TRUE(race.winner->layers().exists(tissue));
    // The winner still holds the writer lock.
    race.winner->close();
    WorkspacePtr reopened = Workspace::open(pathText.c_str(), config);
    EXPECT_TRUE(reopened->layers().find("tissue", tissue));
}

} // namespace

// Two creates on one path both pass the occupancy check; the winner's create runs
// inside the loser's, through the test seam, just before the loser makes anything.
TEST(Workspace, RacingCreateLoserRemovesNothing) {
    const Prepared t;
    // The path did not exist: the loser finds the directory made by someone else.
    expectLoserRemovesNothing(t.workspace, t.slideUtf8(), t.config);

    // The path was an empty directory: the loser's store creation finds the winner's store.
    const fs::path empty = t.dir.path() / "empty";
    fs::create_directories(empty);
    expectLoserRemovesNothing(empty, t.slideUtf8(), t.config);
}

TEST(Workspace, MissingParentIsIoErrorAndMakesNothing) {
    const Prepared t;
    const fs::path parent = t.dir.path() / "missing";
    const fs::path path = parent / "ws";
    try {
        Workspace::create(path.u8string().c_str(), t.slideUtf8().c_str(), 0, t.config);
        FAIL() << "create made a workspace under a missing parent";
    } catch (const IoError& e) {
        EXPECT_NE(std::string(e.what()).find(path.u8string()), std::string::npos) << e.what();
    }
    EXPECT_FALSE(fs::exists(parent));
}

namespace {

// Changes the working directory for one scope.
class CurrentDirectory {
public:
    explicit CurrentDirectory(const fs::path& to) : previous_(fs::current_path()) { fs::current_path(to); }
    ~CurrentDirectory() {
        std::error_code ec;
        fs::current_path(previous_, ec);
    }
    CurrentDirectory(const CurrentDirectory&) = delete;
    CurrentDirectory& operator=(const CurrentDirectory&) = delete;

private:
    fs::path previous_;
};

} // namespace

TEST(Workspace, RelativeSlidePathIsStoredAbsolute) {
    const Prepared t;
    const fs::path sub = t.dir.path() / "sub";
    fs::create_directories(sub);
    const std::string expected = fs::absolute(t.slide).lexically_normal().u8string();
    {
        const CurrentDirectory cd(sub);
        WorkspacePtr ws = Workspace::create(t.workspaceUtf8().c_str(), "../slide.tif", 0, t.config);
        EXPECT_EQ(std::string(ws->slidePath()), expected);
        EXPECT_TRUE(fs::u8path(ws->slidePath()).is_absolute());
        EXPECT_EQ(std::string(ws->store()->provenance().slideId()), expected);
        ws->close();
    }
    // Reopened from another working directory, the binding still names the slide.
    WorkspacePtr ws = Workspace::open(t.workspaceUtf8().c_str(), t.config);
    EXPECT_EQ(std::string(ws->slidePath()), expected);
    ASSERT_TRUE(ws->scene());
    EXPECT_EQ(sceneWidth(ws), 300);
}
