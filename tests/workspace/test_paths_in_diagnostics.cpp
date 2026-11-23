// Requirements: UR-EXE-5, UR-STG-9
// Paths in diagnostics (the rule of 2026-11-23, which revoked "no paths in diagnostics"): an
// exception message, error cause or log line about a specific file or directory names it by
// its path, and third-party text (std::filesystem, SlideIO, libtiff, SQLite) reaches causes and
// logs unaltered. The slide, the workspace and every file below sit in a directory whose name
// holds a marker, and some file names hold it too. Each failure the library can be made to
// report is recorded with its what() (which carries the cause); each log line expectation picks
// the captured lines (every level) that contain a needle. Every row must contain the marker.
#include "common/temp_path.hpp"
#include "pipeline/test_support/synthetic_scene.hpp"
#include "storage/raster_test_util.hpp"
#include "workspace/support/test_slide.hpp"

#include "core/detail/diagnostics.hpp"

#include <slideio/processor/config/configuration.hpp>
#include <slideio/processor/core/data_type.hpp>
#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/geometry.hpp>
#include <slideio/processor/core/log.hpp>
#include <slideio/processor/masks/mask_writer.hpp>
#include <slideio/processor/core/object_query.hpp>
#include <slideio/processor/pipeline/operation_catalog.hpp>
#include <slideio/processor/reporting/csv_writer.hpp>
#include <slideio/processor/reporting/geojson_exporter.hpp>
#include <slideio/processor/storage/event_journal.hpp>
#include <slideio/processor/storage/geojson_import.hpp>
#include <slideio/processor/storage/layer_registry.hpp>
#include <slideio/processor/storage/raster_store.hpp>
#include <slideio/processor/storage/stage_state_writer.hpp>
#include <slideio/processor/storage/store.hpp>
#include <slideio/processor/workspace/workspace.hpp>

#include <slideio/slideio/scene.hpp>
#include <slideio/slideio/slide.hpp>
#include <slideio/slideio/slideio.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>

using namespace slideio::processor;
using testing_support::TempDirectory;
using testing_support::writeTestSlide;

namespace fs = std::filesystem;

namespace {

const std::string kMarker = "PATIENT-MARKER-42";

// Collects every log line, at every level, while it lives.
class CapturedLog {
public:
    CapturedLog() {
        sink_ = std::make_shared<Sink>();
        previousLevel_ = logLevel();
        setLogLevel(LogLevel::Trace);
        setLogSink(sink_);
    }
    CapturedLog(const CapturedLog&) = delete;
    CapturedLog& operator=(const CapturedLog&) = delete;
    ~CapturedLog() {
        setLogSink(nullptr);
        setLogLevel(previousLevel_);
    }

    std::vector<std::string> lines() const {
        std::lock_guard<std::mutex> lock(sink_->mutex);
        return sink_->lines;
    }

private:
    struct Sink : LogSink {
        void write(LogLevel, const char* message) override {
            std::lock_guard<std::mutex> lock(mutex);
            lines.push_back(message == nullptr ? "" : message);
        }
        mutable std::mutex mutex;
        std::vector<std::string> lines;
    };
    std::shared_ptr<Sink> sink_;
    LogLevel previousLevel_;
};

bool carriesMarker(const std::string& text) { return text.find(kMarker) != std::string::npos; }

// One recorded diagnostic: an error's what() or a log line.
struct Row {
    std::string label;
    std::string text;
};

// The log lines that contain `needle`, expected to name their file.
struct LogExpectation {
    std::string label;
    std::string needle;
    bool mustAppear;  // an expectation with no matching line fails when set
};

// Every diagnostic the test has seen.
struct Collected {
    std::vector<Row> rows;
    std::vector<LogExpectation> logExpectations;
    int expectedFailures = 0;

    // Runs `action`, which must throw; records what() (message and cause) of the error.
    void failure(const char* label, const std::function<void()>& action) {
        try {
            action();
        } catch (const std::exception& e) {
            rows.push_back(Row{label, e.what()});
            ++expectedFailures;
            return;
        }
        ADD_FAILURE() << label << " did not throw";
    }

    // Expects the log lines that contain `needle` to carry the marker.
    void logLine(const std::string& label, const std::string& needle, bool mustAppear) {
        logExpectations.push_back(LogExpectation{label, needle, mustAppear});
    }
};

// The little-endian unsigned integer of `size` bytes at `at`.
uint32_t readLe(const std::string& bytes, size_t at, size_t size) {
    uint32_t value = 0;
    for (size_t i = 0; i < size; ++i) {
        value |= static_cast<uint32_t>(static_cast<uint8_t>(bytes[at + i])) << (8 * i);
    }
    return value;
}

// Points every tile offset of the first page of a little-endian classic TIFF (as
// writeTestSlide writes it) past the end of the file: the slide still opens, and reading
// any tile fails. Done before the slide is opened, as an open file may be mapped.
void pointTileOffsetsPastTheEnd(const fs::path& file) {
    std::string bytes;
    {
        std::ifstream in(file, std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    ASSERT_GE(bytes.size(), 8u);
    ASSERT_EQ(bytes.substr(0, 4), std::string("II*\0", 4));
    const size_t ifd = readLe(bytes, 4, 4);
    const size_t entries = readLe(bytes, ifd, 2);
    bool found = false;
    for (size_t e = 0; e < entries; ++e) {
        const size_t entry = ifd + 2 + 12 * e;
        const uint32_t tag = readLe(bytes, entry, 2);
        if (tag != 324) continue;  // TileOffsets
        const uint32_t type = readLe(bytes, entry + 2, 2);
        ASSERT_EQ(type, 4u) << "TileOffsets are not LONG";
        const size_t count = readLe(bytes, entry + 4, 4);
        const size_t values = count == 1 ? entry + 8 : readLe(bytes, entry + 8, 4);
        for (size_t i = 0; i < count; ++i) {
            for (size_t b = 0; b < 4; ++b) bytes[values + 4 * i + b] = static_cast<char>(b == 3 ? 0x70 : 0x00);
        }
        found = true;
    }
    ASSERT_TRUE(found) << "no TileOffsets tag";
    writeFile(file, bytes);
}

// A scene that reports a slide file and whose reads fail with that path in SlideIO's text,
// as some SlideIO drivers word their read errors.
std::shared_ptr<slideio::Scene> sceneWhoseReadsNameItsFile(const std::string& reported) {
    auto source = std::make_shared<testing_support::SyntheticSource>(
        std::vector<testing_support::SyntheticLevel>{{128, 96, 64, 64}},
        std::vector<slideio::DataType>{slideio::DataType::DT_Byte}, [](int, int64_t, int64_t) { return 1.0; });
    slideio::SceneMetadata metadata = source->metadata();
    metadata.reportedFilePath = reported;
    std::shared_ptr<slideio::Scene> scene = slideio::makeScene(source, metadata);
    source->failReadsWith("cannot read a tile of the file " + reported);
    return scene;
}

}  // namespace

TEST(Diagnostics, WithPathAppendsThePath) {
    EXPECT_EQ(detail::withPath("cannot open the store file", "D:/x/s.db"), "cannot open the store file (D:/x/s.db)");
}

TEST(Diagnostics, FilesystemErrorKeepsItsPaths) {
    const std::filesystem::filesystem_error error("cannot remove", fs::u8path("a/" + kMarker),
                                                  std::make_error_code(std::errc::permission_denied));
    const std::string text = detail::diagnosticText(error);
    EXPECT_EQ(text, error.what());
    EXPECT_NE(text.find(kMarker), std::string::npos) << text;
}

TEST(Diagnostics, SlideCauseIsNotRedacted) {
    const Configuration config = Configuration::defaults();
    TempDirectory root;
    const fs::path dir = root.path() / kMarker;
    fs::create_directories(dir);

    // A missing slide: the message names the slide, and SlideIO's text, unaltered, is cause().
    const std::string workspace = (dir / "ws").u8string();
    const std::string missing = (dir / ("missing-" + kMarker + ".tif")).u8string();
    try {
        Workspace::create(workspace.c_str(), missing.c_str(), 0, config);
        FAIL() << "Workspace::create succeeded with a missing slide";
    } catch (const IoError& e) {
        const std::string text = e.what();
        const std::string cause = e.cause();
        EXPECT_FALSE(cause.empty()) << text;
        EXPECT_EQ(text, "Workspace::create: cannot open the slide (" + missing + "); cause: " + cause);
        EXPECT_NE(cause, "<slide>") << text;
        EXPECT_EQ(cause.find("<slide>"), std::string::npos) << text;
        // SlideIO words its "no driver" error with the path it was given.
        EXPECT_NE(cause.find(missing), std::string::npos) << text;
    }

    // A scene whose reads fail with its file's path in SlideIO's text: cause() is that text,
    // unaltered.
    const std::string reported = (dir / ("reported-" + kMarker + ".svs")).u8string();
    std::shared_ptr<slideio::Scene> scene = sceneWhoseReadsNameItsFile(reported);
    const std::string maskOut = (dir / "mask-out.ome.tif").u8string();
    try {
        MaskWriter(maskOut.c_str()).write(scene, nullptr, CancellationToken());
        FAIL() << "MaskWriter succeeded over a scene whose reads fail";
    } catch (const IoError& e) {
        ASSERT_NE(e.cause(), nullptr) << e.what();
        EXPECT_EQ(std::string(e.cause()), "cannot read a tile of the file " + reported) << e.what();
        // The mask writer's own message names the slide the scene reports.
        EXPECT_EQ(std::string(e.what()), "MaskWriter: SlideIO failed to read the scene (" + reported +
                                             "); cause: cannot read a tile of the file " + reported);
    }
}

TEST(Diagnostics, PathsInErrorsAndLogs) {
    CapturedLog log;
    Collected seen;
    const Configuration config = Configuration::defaults();

    TempDirectory root;
    const fs::path dir = root.path() / kMarker;
    fs::create_directories(dir);
    const fs::path slide = writeTestSlide(dir / "slide-PATIENT-MARKER-42.tif", 300, 200);
    const std::string slideText = slide.u8string();
    const auto u8 = [](const fs::path& p) { return p.u8string(); };

    // The store: missing, over an existing one, read-only for writing, held, closed.
    const std::string missingStore = u8(dir / "missing.store");
    seen.failure("Store::open of a missing store",
                 [&] { Store::open(missingStore.c_str(), OpenMode::ReadWrite, config); });
    seen.failure("Store::open ReadOnly of a missing store",
                 [&] { Store::open(missingStore.c_str(), OpenMode::ReadOnly, config); });
    const std::string storeText = u8(dir / "a.store");
    {
        StorePtr store = Store::open(storeText.c_str(), OpenMode::CreateNew, config);
        seen.failure("CreateNew over an existing store",
                     [&] { Store::open(storeText.c_str(), OpenMode::CreateNew, config); });
        seen.failure("a second writer", [&] { Store::open(storeText.c_str(), OpenMode::ReadWrite, config); });
        store->layers().declareSemantic("cells", store->layers().pixelLayer());
        store->stageStateWriter().declare("imp", "");
        store->close();
        seen.failure("a closed store", [&] { store->layers().pixelLayer(); store->provenance(); });
        seen.failure("a closed store's rasters",
                     [&] { store->rasters().declare(RasterDeclaration("r", DataType::Byte, 1)); });
    }
    {
        StorePtr readOnly = Store::open(storeText.c_str(), OpenMode::ReadOnly, config);
        seen.failure("a read-only store declaring a raster",
                     [&] { readOnly->rasters().declare(RasterDeclaration("r", DataType::Byte, 1)); });
        seen.failure("a read-only store declaring a stage", [&] { readOnly->stageStateWriter().declare("s", ""); });
        seen.failure("a read-only store's importer", [&] {
            LayerId cells;
            readOnly->layers().find("cells", cells);
            GeoJsonImporter importer(*readOnly, cells, StageId(1));
        });
        EventCursorPtr cursor = readOnly->events().subscribe(0);
        cursor->close();
        seen.failure("a closed event cursor", [&] { cursor->next(0); });
    }
    // A file that is not a store.
    const fs::path garbage = dir / "garbage.store";
    writeFile(garbage, std::string(8192, 'x'));
    const std::string garbageText = u8(garbage);
    seen.failure("a file that is not a store",
                 [&] { Store::open(garbageText.c_str(), OpenMode::ReadWrite, config); });
    seen.failure("a file that is not a store, read-only",
                 [&] { Store::open(garbageText.c_str(), OpenMode::ReadOnly, config); });

    // A corrupt raster file, read through RasterRegistry::sourceFor.
    {
        const std::string rasterStore = u8(dir / "r.store");
        StorePtr store = Store::open(rasterStore.c_str(), OpenMode::CreateNew, config);
        const StageId stage = store->stageStateWriter().declare("segment", "{}");
        RasterRegistry& rasters = store->rasters();
        const RasterId raster = rasters.declare(RasterDeclaration("gradient", DataType::Int32, 1));
        {
            RasterWriterPtr writer = rasters.writerForStage(stage, raster);
            writer->writeTile(makeView(0, Rect64{0, 0, 64, 64}, 64, 64, DataType::Int32, 1, 3.0)->view);
            writer->flush();
        }
        const fs::path directory = fs::u8path(rasters.directory());
        int corrupted = 0;
        for (const auto& entry : fs::directory_iterator(directory)) {
            if (entry.path().extension() == ".tif") {
                writeFile(entry.path(), std::string(4096, 'x'));
                ++corrupted;
            }
        }
        ASSERT_EQ(corrupted, 1);
        seen.failure("a corrupt raster file", [&] {
            TileRasterSourcePtr source = rasters.sourceFor(raster);
            auto owned = makeView(0, Rect64{0, 0, 64, 64}, 64, 64, source->planeType(), source->planeCount(), 7.0);
            source->fillTile(owned->view);
        });
    }

    // Log lines: an unflushed raster writer, features the importer skips, a reopened workspace.
    {
        const std::string logStore = u8(dir / "l.store");
        StorePtr store = Store::open(logStore.c_str(), OpenMode::CreateNew, config);
        const StageId stage = store->stageStateWriter().declare("segment", "{}");
        const RasterId raster = store->rasters().declare(RasterDeclaration("abandoned", DataType::Int32, 1));
        {
            RasterWriterPtr writer = store->rasters().writerForStage(stage, raster);
            writer->writeTile(makeView(0, Rect64{0, 0, 64, 64}, 64, 64, DataType::Int32, 1, 3.0)->view);
        }  // destroyed unflushed: logs a warning
        seen.logLine("an unflushed raster writer", "RasterWriter:", true);
        const LayerId cells = store->layers().declareSemantic("cells", store->layers().pixelLayer());
        const StageId imp = store->stageStateWriter().declare("imp", "");
        const fs::path skipping = dir / "skipping.geojson";
        writeFile(skipping,
                  R"({"type":"FeatureCollection","features":[{"type":"Feature","properties":{},"geometry":{"type":"Point","coordinates":[1,2]}},)"
                  R"({"type":"Feature","properties":{},"geometry":{"type":"Polygon","coordinates":[[[0,0],[5,0],[0,0]]]}},)"
                  R"({"type":"Feature","properties":{},"geometry":{"type":"Polygon","coordinates":[[[0,0],[10,0],[10,10],[0,10],[0,0]]]}}]})");
        GeoJsonImporter(*store, cells, imp).import(u8(skipping).c_str());
        seen.logLine("a feature the reader skips (GeoJsonReader's line)", "GeoJsonReader: skipping feature", true);
        // The degenerate polygon: the reader rejects its geometry, and the importer logs the skip.
        seen.logLine("a feature the importer skips for its geometry (GeoJsonImporter's line)",
                     "GeoJsonImporter::import: skipping feature", true);
        const std::string reopened = u8(dir / "ws-reopened");
        Workspace::create(reopened.c_str(), slideText.c_str(), 0, config)->close();
        Workspace::open(reopened.c_str(), config)->close();
        seen.logLine("a reopened workspace's sweep", "reclaimed", true);
    }

    // GeoJSON: a missing file, a file that is not GeoJSON, an unknown label, a file without a polygon.
    {
        const std::string importStore = u8(dir / "i.store");
        StorePtr store = Store::open(importStore.c_str(), OpenMode::CreateNew, config);
        const LayerId cells = store->layers().declareSemantic("cells", store->layers().pixelLayer());
        const StageId stage = store->stageStateWriter().declare("imp", "");
        GeoJsonImporter importer(*store, cells, stage);
        const std::string missingJson = u8(dir / "missing.geojson");
        seen.failure("a GeoJSON import of a missing file", [&] { importer.import(missingJson.c_str()); });
        seen.failure("a GeoJSON ROI of a missing file", [&] { importRoiPolygon(missingJson.c_str()); });
        const fs::path notJson = dir / "not.geojson";
        writeFile(notJson, "this is not json");
        const std::string notJsonText = u8(notJson);
        seen.failure("a GeoJSON import of a file that is not GeoJSON", [&] { importer.import(notJsonText.c_str()); });
        seen.failure("a GeoJSON ROI of a file that is not GeoJSON", [&] { importRoiPolygon(notJsonText.c_str()); });
        const fs::path noPolygon = dir / "empty.geojson";
        writeFile(noPolygon, R"({"type":"FeatureCollection","features":[]})");
        const std::string noPolygonText = u8(noPolygon);
        seen.failure("a GeoJSON ROI without a polygon", [&] { importRoiPolygon(noPolygonText.c_str()); });
        const fs::path unknownLabel = dir / "label.geojson";
        writeFile(unknownLabel,
                  R"({"type":"FeatureCollection","features":[{"type":"Feature","properties":{"classification":{"name":"zzz-unknown"}},)"
                  R"("geometry":{"type":"Polygon","coordinates":[[[0,0],[10,0],[10,10],[0,10],[0,0]]]}}]})");
        const std::string unknownLabelText = u8(unknownLabel);
        seen.failure("a GeoJSON import with an unknown label", [&] { importer.import(unknownLabelText.c_str()); });
        // Exporters writing into a directory that does not exist.
        const std::string noDir = u8(dir / "nowhere" / "out.csv");
        seen.failure("a CSV export into a missing directory", [&] {
            CsvWriter(noDir.c_str()).write(*store, ObjectQuery());
        });
        const std::string noDirJson = u8(dir / "nowhere" / "out.geojson");
        seen.failure("a GeoJSON export into a missing directory", [&] {
            GeoJsonExporter(noDirJson.c_str()).write(*store, ObjectQuery());
        });
        const std::string matrix = u8(dir / "nowhere" / "matrix.md");
        seen.failure("a support matrix into a missing directory",
                     [&] { OperationCatalog::instance().writeSupportMatrix(matrix.c_str()); });
    }

    // The workspace: a missing slide, a scene out of range, a missing workspace.
    const std::string workspaceText = u8(dir / "ws");
    const std::string missingSlide = u8(dir / "missing-slide.tif");
    seen.failure("Workspace::create with a missing slide",
                 [&] { Workspace::create(workspaceText.c_str(), missingSlide.c_str(), 0, config); });
    seen.failure("Workspace::create with a scene out of range",
                 [&] { Workspace::create(workspaceText.c_str(), slideText.c_str(), 5, config); });
    seen.failure("Workspace::open of a missing workspace",
                 [&] { Workspace::open(workspaceText.c_str(), config); });
    seen.failure("Workspace::openReadOnly of a missing workspace",
                 [&] { Workspace::openReadOnly(workspaceText.c_str(), config); });
    fs::create_directories(dir / "ws-full");
    writeFile(dir / "ws-full" / "x.txt", "x");
    const std::string full = u8(dir / "ws-full");
    seen.failure("Workspace::create over a non-empty directory",
                 [&] { Workspace::create(full.c_str(), slideText.c_str(), 0, config); });
    fs::create_directories(dir / "ws-empty");
    const std::string emptyDir = u8(dir / "ws-empty");
    seen.failure("Workspace::open of an empty directory", [&] { Workspace::open(emptyDir.c_str(), config); });

    // A workspace whose recorded slide has moved.
    {
        const std::string moved = u8(dir / "ws-moved");
        const fs::path slideCopy = writeTestSlide(dir / "moving-slide.tif", 300, 200);
        Workspace::create(moved.c_str(), u8(slideCopy).c_str(), 0, config)->close();
        fs::remove(slideCopy);
        seen.failure("Workspace::open whose slide is gone", [&] { Workspace::open(moved.c_str(), config); });
        seen.failure("Workspace::openReadOnly whose slide is gone",
                     [&] { Workspace::openReadOnly(moved.c_str(), config); });
    }


    // MaskWriter over a file-backed scene of a corrupt slide (its tiles lie past the end of
    // the file, so it opens but no tile reads).
    {
        const fs::path corrupt = writeTestSlide(dir / "corrupt-PATIENT-MARKER-42.tif", 300, 200);
        pointTileOffsetsPastTheEnd(corrupt);
        std::shared_ptr<slideio::Slide> opened = slideio::openSlide(u8(corrupt), "");
        std::shared_ptr<slideio::Scene> scene = opened->getScene(0);
        const std::string maskOut = u8(dir / "mask-out.ome.tif");
        seen.failure("MaskWriter over a corrupt slide",
                     [&] { MaskWriter(maskOut.c_str()).write(scene, nullptr, CancellationToken()); });
    }
    // The same through a scene that reports a slide file and whose reads fail with the path
    // in SlideIO's text: the text reaches the cause unaltered.
    {
        std::shared_ptr<slideio::Scene> scene = sceneWhoseReadsNameItsFile(u8(dir / "reported-PATIENT-MARKER-42.svs"));
        const std::string maskOut = u8(dir / "mask-out-2.ome.tif");
        seen.failure("MaskWriter over a scene whose reads name its file",
                     [&] { MaskWriter(maskOut.c_str()).write(scene, nullptr, CancellationToken()); });
    }

    EXPECT_GE(seen.expectedFailures, 25);
    const std::vector<std::string> lines = log.lines();
    EXPECT_GE(lines.size(), 3u) << "the log capture saw too little for the check to mean anything";

    std::vector<std::string> offenders;
    const auto check = [&](const std::string& kind, const Row& row) {
        if (!carriesMarker(row.text)) offenders.push_back(kind + row.label + " -> " + row.text);
    };
    for (const Row& row : seen.rows) check("ERROR: ", row);
    for (const LogExpectation& expectation : seen.logExpectations) {
        bool matched = false;
        for (const std::string& line : lines) {
            if (line.find(expectation.needle) == std::string::npos) continue;
            matched = true;
            check("LOG: ", Row{expectation.label, line});
        }
        if (!matched && expectation.mustAppear) {
            offenders.push_back("LOG: " + expectation.label + " -> no line contains \"" + expectation.needle + "\"");
        }
    }

    std::string report;
    for (const std::string& offender : offenders) report += "\n  " + offender;
    EXPECT_TRUE(offenders.empty()) << offenders.size() << " diagnostics name no path:" << report;
}
