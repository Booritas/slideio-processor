// Requirements: UR-API-2, UR-RES-7, UR-RES-8, UR-RES-9, UR-RES-11
#include "workspace/support/test_slide.hpp"

#include <slideio/processor/config/configuration.hpp>
#include <slideio/processor/core/control.hpp>
#include <slideio/processor/core/error.hpp>
#include <slideio/processor/pipeline/operation_parameters.hpp>
#include <slideio/processor/pipeline/pipeline_spec.hpp>
#include <slideio/processor/run/run.hpp>
#include <slideio/processor/storage/raster_store.hpp>
#include <slideio/processor/storage/store.hpp>
#include <slideio/processor/workspace/workspace.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <sstream>
#include <string>

using namespace slideio::processor;
namespace fs = std::filesystem;
namespace ts = slideio::processor::testing_support;

namespace {

std::string pipelineJson(const PipelineSpec& spec) {
    const int32_t length = spec.toJson(nullptr, 0);
    std::string text(static_cast<size_t>(length) + 1, '\0');
    spec.toJson(&text[0], length + 1);
    text.resize(static_cast<size_t>(length));
    return text;
}

std::string record(const std::string& name, const std::string& writes, const std::string& restriction,
                   const std::string& raster, const PipelineSpec& pipeline) {
    return "{\"stageRecordVersion\":1,\"stage\":\"" + name + "\",\"readsLayers\":[],\"writesLayers\":[" + writes +
           "],\"annotatesLayers\":[],\"restrictToLayer\":" + restriction +
           ",\"readsLayersAsRaster\":[],\"writesRaster\":" + raster +
           ",\"readsRasters\":[],\"dependsOn\":[],\"pipeline\":" + pipelineJson(pipeline) + "}";
}

PipelineSpec blobChain() {
    PipelineSpec spec;
    spec.operation("pixel.colour_convert", OperationParameters().setString("from", "Rgb").setString("to", "Grey"));
    spec.operation("pixel.threshold_fixed",
                   OperationParameters().setDouble("value", 128.0).setString("direction", "Below"));
    spec.operation("segment.connected_components", OperationParameters());
    return spec;
}

PipelineSpec probChain() {
    PipelineSpec spec;
    spec.operation("pixel.colour_convert", OperationParameters().setString("from", "Rgb").setString("to", "Grey"));
    spec.operation("pixel.gaussian_blur", OperationParameters());
    return spec;
}

// The three-stage file of the slide-run tests: tissue, nuclei restricted to it, and prob.
PipelineFile threeStageFile() {
    const std::string stages = record("tissue", "\"tissue\"", "null", "null", blobChain()) + "," +
                               record("nuclei", "\"nuclei\"", "\"tissue\"", "null", blobChain()) + "," +
                               record("prob", "", "null", "\"prob\"", probChain());
    const std::string json =
        std::string("{\"formatVersion\":1,\"requires\":[],"
                    "\"layers\":[{\"name\":\"tissue\"},{\"name\":\"nuclei\",\"parent\":\"tissue\"}],"
                    "\"rasters\":[{\"name\":\"prob\",\"type\":\"Float32\",\"planes\":1}],"
                    "\"stages\":[") +
        stages +
        "],\"exports\":["
        "{\"kind\":\"csv\",\"file\":\"nuclei.csv\",\"layer\":\"nuclei\"},"
        "{\"kind\":\"geojson\",\"file\":\"nuclei.geojson\",\"layer\":\"nuclei\"},"
        "{\"kind\":\"labelMask\",\"file\":\"nuclei-mask.ome.tif\",\"layer\":\"nuclei\"},"
        "{\"kind\":\"rasterTiff\",\"file\":\"prob.ome.tif\",\"raster\":\"prob\"}]}";
    return PipelineFile::fromJson(json.c_str());
}

std::string readFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// The text without its "#" lines: the provenance comments, which carry the store's creation time.
std::string withoutComments(const std::string& text) {
    std::istringstream in(text);
    std::string line;
    std::string kept;
    while (std::getline(in, line)) {
        if (!line.empty() && line[0] == '#') continue;
        kept += line + "\n";
    }
    return kept;
}

// A blob slide run through the three-stage file by SlideRun, so a workspace and the run's own
// exports exist; each test opens the workspace afresh.
struct Fixture {
    Fixture() {
        fs::create_directories(dir.path());
        const std::string slide = ts::writeBlobSlide(dir.path() / "slide.tif", 600, 600).u8string();
        workspace = (dir.path() / "ws").u8string();
        out = (dir.path() / "out").u8string();
        file = threeStageFile();
        SlideRun(file, slide.c_str(), 0, workspace.c_str(), out.c_str(), Configuration::defaults()).run();
        fs::create_directories(dir.path() / "again");
    }
    WorkspacePtr openReadOnly() const { return Workspace::openReadOnly(workspace.c_str(), Configuration::defaults()); }
    std::string again(const char* name) const { return (dir.path() / "again" / name).u8string(); }

    ts::TempDirectory dir;
    std::string workspace;
    std::string out;
    PipelineFile file;
};

void expectRefused(const std::function<void()>& call, const std::string& name) {
    try {
        call();
        FAIL() << "expected InvalidArgumentError naming " << name;
    } catch (const InvalidArgumentError& e) {
        const std::string what = e.what();
        EXPECT_EQ(what.rfind("writeExport: ", 0), 0u) << what;
        EXPECT_NE(what.find(name), std::string::npos) << what;
    }
}

} // namespace

TEST(WorkspaceExport, CsvMatchesSlideRun) {
    Fixture f;
    const WorkspacePtr ws = f.openReadOnly();
    const std::string path = f.again("nuclei.csv");
    const ExportResult result =
        writeExport(*ws, f.file.exportAt(0), path.c_str(), nullptr, nullptr, CancellationToken());
    EXPECT_FALSE(result.wasCancelled());
    const std::string written = withoutComments(readFile(fs::u8path(path)));
    ASSERT_FALSE(written.empty());
    EXPECT_EQ(written, withoutComments(readFile(fs::u8path(f.out) / "nuclei.csv")));
}

TEST(WorkspaceExport, EachKindWrites) {
    Fixture f;
    const WorkspacePtr ws = f.openReadOnly();
    const char* names[] = {"nuclei.csv", "nuclei.geojson", "nuclei-mask.ome.tif", "prob.ome.tif"};
    for (int32_t i = 1; i < 4; ++i) {
        const std::string path = f.again(names[i]);
        const ExportResult result =
            writeExport(*ws, f.file.exportAt(i), path.c_str(), "slide-1", nullptr, CancellationToken());
        EXPECT_GT(result.bytesWritten(), 0) << names[i];
        EXPECT_TRUE(fs::exists(fs::u8path(path))) << names[i];
        EXPECT_FALSE(fs::exists(fs::u8path(path + ".partial"))) << names[i];
    }
}

TEST(WorkspaceExport, ReadOnlyWorkspace) {
    Fixture f;
    const WorkspacePtr ws = f.openReadOnly();
    ASSERT_TRUE(ws->isReadOnly());
    const std::string path = f.again("nuclei.csv");
    const ExportResult result = writeExport(*ws, f.file.exportAt(0), path.c_str(), "", nullptr, CancellationToken());
    EXPECT_GT(result.bytesWritten(), 0);
}

TEST(WorkspaceExport, UnknownLayerRefused) {
    Fixture f;
    const WorkspacePtr ws = f.openReadOnly();
    const std::string path = f.again("x.csv");
    expectRefused(
        [&] {
            writeExport(*ws, ExportSpec::csv("x.csv", "ghost-layer"), path.c_str(), nullptr, nullptr,
                        CancellationToken());
        },
        "ghost-layer");
    EXPECT_FALSE(fs::exists(fs::u8path(path)));
}

TEST(WorkspaceExport, UnknownRasterRefused) {
    Fixture f;
    const WorkspacePtr ws = f.openReadOnly();
    const std::string path = f.again("x.ome.tif");
    expectRefused(
        [&] {
            writeExport(*ws, ExportSpec::rasterTiff("x.ome.tif", "ghost-raster"), path.c_str(), nullptr, nullptr,
                        CancellationToken());
        },
        "ghost-raster");
}

TEST(WorkspaceExport, UnwrittenRasterRefused) {
    Fixture f;
    {
        WorkspacePtr writable = Workspace::open(f.workspace.c_str(), Configuration::defaults());
        writable->store()->rasters().declare(RasterDeclaration("idle", DataType::Float32, 1));
        writable->close();
    }
    const WorkspacePtr ws = f.openReadOnly();
    const std::string path = f.again("idle.ome.tif");
    expectRefused(
        [&] {
            writeExport(*ws, ExportSpec::rasterTiff("idle.ome.tif", "idle"), path.c_str(), nullptr, nullptr,
                        CancellationToken());
        },
        "idle");
}

TEST(WorkspaceExport, EmptyPathRefused) {
    Fixture f;
    const WorkspacePtr ws = f.openReadOnly();
    EXPECT_THROW(writeExport(*ws, f.file.exportAt(0), nullptr, nullptr, nullptr, CancellationToken()),
                 InvalidArgumentError);
    EXPECT_THROW(writeExport(*ws, f.file.exportAt(0), "", nullptr, nullptr, CancellationToken()),
                 InvalidArgumentError);
}

TEST(WorkspaceExport, CancelledLeavesNoFile) {
    Fixture f;
    const WorkspacePtr ws = f.openReadOnly();
    CancellationSource source;
    source.cancel();
    const std::string path = f.again("nuclei.csv");
    const ExportResult result = writeExport(*ws, f.file.exportAt(0), path.c_str(), nullptr, nullptr, source.token());
    EXPECT_TRUE(result.wasCancelled());
    EXPECT_FALSE(fs::exists(fs::u8path(path)));
    EXPECT_FALSE(fs::exists(fs::u8path(path + ".partial")));
}
