// Requirements: UR-EXE-5, UR-STG-9
// No exception message, error cause or log line may contain a file path. Slide file names
// often carry patient identifiers, and the bindings' errors and logs can reach a language
// model. The slide, the workspace and every file below sit in directories whose names hold a
// marker; every failure the library can be made to report is collected with its cause (what()
// carries the cause), and so is every log line at Debug level. None may contain the marker.
#include "common/temp_path.hpp"
#include "storage/raster_test_util.hpp"
#include "workspace/support/test_slide.hpp"

#include <slideio/processor/config/configuration.hpp>
#include <slideio/processor/core/data_type.hpp>
#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/geometry.hpp>
#include <slideio/processor/core/log.hpp>
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

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
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

// Every text the test has seen, labelled with how it arose.
struct Collected {
    std::vector<std::string> texts;
    int expectedFailures = 0;

    // Runs `action`, which must throw; records what() (message and cause) of the error.
    void failure(const char* label, const std::function<void()>& action) {
        try {
            action();
        } catch (const std::exception& e) {
            texts.push_back(std::string(label) + " -> " + e.what());
            ++expectedFailures;
            return;
        }
        ADD_FAILURE() << label << " did not throw";
    }
};

bool leaks(const std::string& text) { return text.find(kMarker) != std::string::npos; }

}  // namespace

TEST(Diagnostics, NoPathsInErrorsOrLogs) {
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
        seen.failure("a closed store's rasters", [&] { store->rasters().declare(RasterDeclaration("r", DataType::Byte, 1)); });
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
        const LayerId cells = store->layers().declareSemantic("cells", store->layers().pixelLayer());
        const StageId imp = store->stageStateWriter().declare("imp", "");
        const fs::path skipping = dir / "skipping.geojson";
        writeFile(skipping,
                  R"({"type":"FeatureCollection","features":[{"type":"Feature","properties":{},"geometry":{"type":"Point","coordinates":[1,2]}},)"
                  R"({"type":"Feature","properties":{},"geometry":{"type":"Polygon","coordinates":[[[0,0],[10,0],[10,10],[0,10],[0,0]]]}}]})");
        GeoJsonImporter(*store, cells, imp).import(u8(skipping).c_str());
        const std::string reopened = u8(dir / "ws-reopened");
        Workspace::create(reopened.c_str(), slideText.c_str(), 0, config)->close();
        Workspace::open(reopened.c_str(), config)->close();
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

    // Closing a workspace whose directory was removed underneath it: the error, then the
    // destructor's log line.
    {
        const std::string doomed = u8(dir / "ws-doomed");
        WorkspacePtr ws = Workspace::create(doomed.c_str(), slideText.c_str(), 0, config);
        std::error_code ec;
        fs::remove_all(dir / "ws-doomed", ec);
        try {
            ws->close();
        } catch (const std::exception& e) {
            seen.texts.push_back(std::string("closing a workspace whose directory is gone -> ") + e.what());
        }
        ws.reset();
    }
    {
        const std::string doomed = u8(dir / "ws-doomed2");
        WorkspacePtr ws = Workspace::create(doomed.c_str(), slideText.c_str(), 0, config);
        std::error_code ec;
        fs::remove_all(dir / "ws-doomed2", ec);
        ws.reset();  // the destructor logs a failed close, if there is one
    }

    EXPECT_GE(seen.expectedFailures, 25);
    EXPECT_GE(log.lines().size(), 3u) << "the log capture saw too little for the check to mean anything";
    std::vector<std::string> offenders;
    for (const std::string& text : seen.texts) {
        if (leaks(text)) offenders.push_back("ERROR: " + text);
    }
    for (const std::string& line : log.lines()) {
        if (leaks(line)) offenders.push_back("LOG: " + line);
    }
    std::string report;
    for (const std::string& offender : offenders) report += "\n  " + offender;
    EXPECT_TRUE(offenders.empty()) << offenders.size() << " diagnostics carry a path:" << report;
}
