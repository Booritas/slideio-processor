// Requirements: UR-RES-8, UR-RES-13, UR-RES-19, UR-IMG-5
#include "storage/store_fixture.hpp"

#include "common/temp_path.hpp"

#include <slideio/processor/config/configuration.hpp>
#include <slideio/processor/core/control.hpp>
#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/geojson.hpp>
#include <slideio/processor/core/geometry.hpp>
#include <slideio/processor/core/log.hpp>
#include <slideio/processor/core/measurement.hpp>
#include <slideio/processor/core/object_query.hpp>
#include <slideio/processor/core/object_record.hpp>
#include <slideio/processor/core/provenance.hpp>
#include <slideio/processor/storage/event_journal.hpp>
#include <slideio/processor/storage/geojson_import.hpp>
#include <slideio/processor/storage/layer_registry.hpp>
#include <slideio/processor/storage/object_store.hpp>
#include <slideio/processor/storage/store.hpp>
#include <slideio/processor/storage/vocabulary_registry.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

using namespace slideio::processor;

static_assert(std::is_copy_constructible<ImportResult>::value, "ImportResult is a value class");
static_assert(std::is_nothrow_move_constructible<ImportResult>::value, "ImportResult is movable");
static_assert(std::is_nothrow_move_assignable<ImportResult>::value, "ImportResult is movable");
static_assert(!std::is_copy_constructible<GeoJsonImporter>::value, "GeoJsonImporter is not copyable");

namespace {

// The square [x, x + 10) x [y, y + 10) as a closed GeoJSON ring.
std::string squareCoordinates(int64_t x, int64_t y) {
    const std::string x0 = std::to_string(x), x1 = std::to_string(x + 10);
    const std::string y0 = std::to_string(y), y1 = std::to_string(y + 10);
    return "[[[" + x0 + "," + y0 + "],[" + x1 + "," + y0 + "],[" + x1 + "," + y1 + "],[" + x0 + "," + y1 + "],[" +
           x0 + "," + y0 + "]]]";
}

// A Polygon feature over squareCoordinates(x, y) with `properties` as the members of its
// "properties" object.
std::string squareFeature(int64_t x, int64_t y, const std::string& properties = "") {
    return R"({"type":"Feature","geometry":{"type":"Polygon","coordinates":)" + squareCoordinates(x, y) +
           R"(},"properties":{)" + properties + "}}";
}

std::string pointFeature() {
    return R"({"type":"Feature","geometry":{"type":"Point","coordinates":[5,5]},"properties":{}})";
}

// A QuPath-shaped classification property.
std::string qupathClass(const std::string& name) {
    return R"("objectType":"annotation","classification":{"name":")" + name + R"(","color":[200,0,0]})";
}

std::string collection(const std::vector<std::string>& features) {
    std::string out = R"({"type":"FeatureCollection","features":[)";
    for (size_t i = 0; i < features.size(); ++i) out += (i == 0 ? "" : ",\n") + features[i];
    return out + "]}";
}

// The object import makes of squareFeature(x, y) in `layer`.
ObjectRecord squareRecord(LayerId layer, int64_t x, int64_t y) {
    const Point64 points[] = {{x, y}, {x + 10, y}, {x + 10, y + 10}, {x, y + 10}};
    Polygon polygon;
    polygon.addRing(points, 4, false);
    ObjectRecord record;
    record.setLayer(layer);
    record.setGeometry(polygon);
    return record;
}

// A GeoJSON file in the temp directory, removed when the guard goes.
class GeoJsonFile {
public:
    explicit GeoJsonFile(const std::string& content)
        : path_(uniqueTempPath("import", ".geojson")), guard_(path_) {
        writeFile(path_, content);
    }
    std::string utf8() const { return path_.u8string(); }
    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
    TempFileGuard guard_;
};

// Captures log messages; with a trigger, cancels `source` on the first message containing it.
class CaptureSink : public LogSink {
public:
    void write(LogLevel, const char* message) override {
        messages.emplace_back(message);
        if (source != nullptr && !trigger.empty() && messages.back().find(trigger) != std::string::npos) {
            source->cancel();
        }
    }
    std::vector<std::string> messages;
    CancellationSource* source = nullptr;
    std::string trigger;
};

// Installs a CaptureSink and restores the process-global defaults on exit.
class LogCapture {
public:
    LogCapture() : sink_(std::make_shared<CaptureSink>()) {
        setLogSink(sink_);
        setLogLevel(LogLevel::Warning);
    }
    LogCapture(const LogCapture&) = delete;
    LogCapture& operator=(const LogCapture&) = delete;
    ~LogCapture() {
        setLogSink(nullptr);
        setLogLevel(LogLevel::Warning);
    }
    CaptureSink& sink() { return *sink_; }
    bool anyContains(const std::string& text) const {
        for (const std::string& m : sink_->messages) {
            if (m.find(text) != std::string::npos) return true;
        }
        return false;
    }

private:
    std::shared_ptr<CaptureSink> sink_;
};

struct ProgressCall {
    std::string stage;
    double fraction;
    int64_t completed;
    int64_t total;
};

class RecordingObserver : public ProgressObserver {
public:
    void onProgress(const char* stageName, double fraction, int64_t completed, int64_t total) override {
        calls.push_back({stageName, fraction, completed, total});
    }
    std::vector<ProgressCall> calls;
};

int64_t countAll(ObjectStore& objects, ObjectQuery query) {
    return objects.count(query.visibility(StageVisibility::IncludeInFlight));
}

class GeoJsonImport : public StoreFixture {
protected:
    void SetUp() override {
        StoreFixture::SetUp();
        cells = store().layers().declareSemantic("cells", store().layers().pixelLayer());
        imp = store().stageStateWriter().declare("imp", "");
    }
    void complete(StageId stage) { store().stageStateWriter().setStatus(stage, StageStatus::Complete); }

    // 500 features: "Tumor" on all but the last, which carries "my-weird-class".
    static std::string weirdLastFile() {
        std::vector<std::string> features;
        for (int i = 0; i < 500; ++i) {
            features.push_back(squareFeature(i * 20, 0, qupathClass(i < 499 ? "Tumor" : "my-weird-class")));
        }
        return collection(features);
    }

    LayerId cells;
    StageId imp;
};

} // namespace

TEST_F(GeoJsonImport, LandsInTheTargetLayerAlongsideStageObjects) {  // UR-RES-13 acceptance
    const StageId a = store().stageStateWriter().declare("stage-a", "");
    {
        ObjectWriterPtr writer = store().objects().writerForStage(a);
        writer->emit(squareRecord(cells, 0, 0));
        writer->emit(squareRecord(cells, 100, 0));
        writer->flush();
    }
    const GeoJsonFile file(collection({squareFeature(1000, 0), squareFeature(1100, 0, R"("layer":"other")"),
                                       squareFeature(1200, 0, R"("name":"third")")}));

    GeoJsonImporter importer(store(), cells, imp);
    const ImportResult result = importer.import(file.utf8().c_str());
    complete(a);
    complete(imp);

    EXPECT_EQ(result.objectsImported(), 3);
    EXPECT_EQ(result.featuresSkipped(), 0);
    EXPECT_EQ(result.labelsRegistered(), 0);
    EXPECT_EQ(result.policyApplied(), ImportPolicy::Strict);
    EXPECT_FALSE(result.hasSourceProvenance());
    EXPECT_EQ(store().objects().count(ObjectQuery().layer(cells)), 5);
    CursorPtr cursor = store().objects().query(ObjectQuery().layer(cells).stage(imp));
    int64_t imported = 0;
    while (cursor->next()) {
        EXPECT_EQ(cursor->current().stage(), imp);
        EXPECT_EQ(cursor->current().layer(), cells);
        EXPECT_FALSE(cursor->current().hasLabel());
        ++imported;
    }
    EXPECT_EQ(imported, 3);
    LayerId other;
    EXPECT_FALSE(store().layers().find("other", other));
}

TEST_F(GeoJsonImport, StrictRejectsNamingTheLabelAndWritesNothing) {  // Review Focus 5, UR-RES-19
    const GeoJsonFile file(weirdLastFile());
    const int64_t sequence = store().events().newestSequence();
    GeoJsonImporter importer(store(), cells, imp);
    try {
        importer.import(file.utf8().c_str());
        FAIL() << "a label outside the vocabulary was accepted under Strict";
    } catch (const InvalidArgumentError& e) {
        const std::string message = e.what();
        EXPECT_NE(message.find("my-weird-class"), std::string::npos) << message;
        EXPECT_NE(message.find("499"), std::string::npos) << message;
    }
    EXPECT_EQ(store().objects().totalObjectCount(), 0);
    EXPECT_EQ(store().vocabulary().count(), 10);
    EXPECT_EQ(store().events().newestSequence(), sequence);
}

TEST_F(GeoJsonImport, PermissiveRegistersImportedLabels) {  // UR-RES-19 acceptance
    store().vocabulary().setImportPolicy(ImportPolicy::Permissive);
    const GeoJsonFile file(weirdLastFile());
    const int64_t sequence = store().events().newestSequence();

    GeoJsonImporter importer(store(), cells, imp);
    const ImportResult result = importer.import(file.utf8().c_str());
    complete(imp);

    EXPECT_EQ(result.policyApplied(), ImportPolicy::Permissive);
    EXPECT_EQ(result.objectsImported(), 500);
    EXPECT_EQ(result.labelsRegistered(), 1);
    LabelId weird;
    ASSERT_TRUE(store().vocabulary().find("my-weird-class", weird));
    EXPECT_EQ(store().vocabulary().origin(weird), LabelOrigin::ImportRegistered);
    EXPECT_STREQ(store().vocabulary().name(weird), "my-weird-class");
    EXPECT_EQ(store().vocabulary().count(), 11);
    EXPECT_EQ(store().objects().count(ObjectQuery().layer(cells).label(LabelId(1))), 499);
    EXPECT_EQ(store().objects().count(ObjectQuery().layer(cells).label(weird)), 1);

    // The registration's LabelAdded event, among the import's events.
    EventCursorPtr events = store().events().subscribe(sequence);
    int32_t labelAdded = 0;
    while (events->next(0)) {
        if (events->current().kind() == EventKind::LabelAdded) {
            EXPECT_EQ(events->current().label(), weird);
            ++labelAdded;
        }
    }
    EXPECT_EQ(labelAdded, 1);
}

TEST_F(GeoJsonImport, MeasurementsSkipsAndProvenance) {
    LogCapture log;
    const GeoJsonFile file(collection({
        squareFeature(0, 0, R"("patient":"SECRET-PATIENT-7","measurements":{"Area":12.5,"Bad":"SECRET-VALUE-9"})"),
        squareFeature(100, 0, R"("measurements":[{"name":"Perimeter","value":3.25},{"name":"Count","value":"7"}])"),
        pointFeature(),
        R"({"type":"Feature","geometry":{"type":"Polygon","coordinates":[[[0,0],[5,0],[0,0],[5,0],[0,0]]]},"properties":{}})",
        squareFeature(200, 0),
    }));

    GeoJsonImporter importer(store(), cells, imp);
    const ImportResult result = importer.import(file.utf8().c_str());
    complete(imp);

    EXPECT_EQ(result.objectsImported(), 3);
    EXPECT_EQ(result.featuresSkipped(), 2);
    EXPECT_FALSE(result.hasSourceProvenance());
    EXPECT_STREQ(result.sourceProvenance().slideId(), "");

    ObjectRecord first;
    ASSERT_TRUE(store().objects().get(squareRecord(cells, 0, 0).id(), first));
    Measurement m;
    ASSERT_TRUE(first.measurements().find("Area", m));  // object form
    EXPECT_EQ(m.value(), 12.5);
    EXPECT_EQ(m.unit(), MeasurementUnit::Dimensionless);
    EXPECT_FALSE(first.measurements().find("Bad", m));  // not a number: skipped alone
    EXPECT_EQ(first.measurements().count(), 1);
    ObjectRecord second;
    ASSERT_TRUE(store().objects().get(squareRecord(cells, 100, 0).id(), second));
    ASSERT_TRUE(second.measurements().find("Perimeter", m));  // array form (QuPath before 0.3)
    EXPECT_EQ(m.value(), 3.25);
    EXPECT_EQ(m.unit(), MeasurementUnit::Dimensionless);
    ASSERT_TRUE(second.measurements().find("Count", m));
    EXPECT_EQ(m.value(), 7.0);
    ObjectRecord last;
    EXPECT_TRUE(store().objects().get(squareRecord(cells, 200, 0).id(), last));  // after the invalid one

    // The skipped measurement and the invalid geometry are reported by index and name;
    // no property value reaches a diagnostic.
    bool namedMeasurement = false;
    for (const std::string& message : log.sink().messages) {
        if (message.find("feature 0 ") != std::string::npos && message.find("'Bad'") != std::string::npos) {
            namedMeasurement = true;
        }
    }
    EXPECT_TRUE(namedMeasurement);
    EXPECT_TRUE(log.anyContains("GeoJsonImporter::import: skipping feature 3 "));
    EXPECT_FALSE(log.anyContains("SECRET"));

    // A file from GeoJsonWriter: its provenance is reported, the store's is untouched.
    const std::string storeSlide = store().provenance().slideId();
    const std::filesystem::path written = uniqueTempPath("import_written", ".geojson");
    const TempFileGuard writtenGuard(written);
    {
        Provenance provenance;
        provenance.setSlideId("slide-from-file");
        provenance.setSceneIndex(2);
        GeoJsonWriter writer(written.u8string().c_str());
        writer.begin(provenance);
        writer.writeFeature(squareRecord(cells, 5000, 0), "cells", nullptr, nullptr, 0);
        writer.end();
    }
    const ImportResult fromWriter = importer.import(written.u8string().c_str());
    EXPECT_EQ(fromWriter.objectsImported(), 1);
    ASSERT_TRUE(fromWriter.hasSourceProvenance());
    EXPECT_STREQ(fromWriter.sourceProvenance().slideId(), "slide-from-file");
    EXPECT_EQ(fromWriter.sourceProvenance().sceneIndex(), 2);
    EXPECT_EQ(std::string(store().provenance().slideId()), storeSlide);
    EXPECT_FALSE(log.anyContains("slide-from-file"));

    // ImportResult is a value: copies keep the provenance, a moved-from result is empty.
    ImportResult copy(fromWriter);
    EXPECT_STREQ(copy.sourceProvenance().slideId(), "slide-from-file");
    ImportResult moved(std::move(copy));
    EXPECT_EQ(moved.objectsImported(), 1);
    EXPECT_EQ(copy.objectsImported(), 0);  // NOLINT(bugprone-use-after-move): the moved-from state is specified
    EXPECT_FALSE(copy.hasSourceProvenance());
    EXPECT_STREQ(copy.sourceProvenance().slideId(), "");
    copy = moved;
    EXPECT_TRUE(copy.hasSourceProvenance());
    EXPECT_EQ(ImportResult().policyApplied(), ImportPolicy::Strict);
}

TEST_F(GeoJsonImport, EachSkippedFeatureIsLoggedOnceUnderTheReadersIndex) {
    LogCapture log;
    const GeoJsonFile file(collection({
        "42",
        R"({"type":"Feature","geometry":null,"properties":{}})",
        R"({"type":"Feature","geometry":{"type":"Polygon","coordinates":[[[0,0],[5,0],[0,0]]]},"properties":{}})",
        squareFeature(0, 0, R"("measurements":{"Bad":"SECRET-VALUE-3"})"),
    }));

    GeoJsonImporter importer(store(), cells, imp);
    const ImportResult result = importer.import(file.utf8().c_str());
    complete(imp);
    EXPECT_EQ(result.objectsImported(), 1);
    EXPECT_EQ(result.featuresSkipped(), 2);

    // The pre-pass logs nothing; each feature is named by its position in the feature list,
    // the non-object element included, in the reader's messages and the importer's alike.
    const auto count = [&log](const std::string& text) {
        int n = 0;
        for (const std::string& m : log.sink().messages) n += m.find(text) != std::string::npos ? 1 : 0;
        return n;
    };
    EXPECT_EQ(log.sink().messages.size(), 4u);
    EXPECT_EQ(count("GeoJsonReader: skipping feature 0 "), 1);
    EXPECT_EQ(count("GeoJsonReader: skipping feature 1 "), 1);
    EXPECT_EQ(count("GeoJsonImporter::import: skipping feature 2 "), 1);
    EXPECT_EQ(count("GeoJsonReader: feature 2 "), 1);  // the reader's own error, quoted by the importer
    EXPECT_EQ(count("GeoJsonImporter::import: feature 3 "), 1);
    EXPECT_FALSE(log.anyContains("SECRET"));
}

TEST_F(GeoJsonImport, StrictUnknownLabelIsNamedUnderTheReadersIndex) {  // UR-RES-19
    const GeoJsonFile file(collection({"42", "[1]", squareFeature(0, 0, qupathClass("Tumor")),
                                       squareFeature(20, 0, qupathClass("my-weird-class"))}));
    GeoJsonImporter importer(store(), cells, imp);
    try {
        importer.import(file.utf8().c_str());
        FAIL() << "a label outside the vocabulary was accepted under Strict";
    } catch (const InvalidArgumentError& e) {
        const std::string message = e.what();
        EXPECT_NE(message.find("feature 3 of"), std::string::npos) << message;  // the two non-objects count
        EXPECT_NE(message.find("my-weird-class"), std::string::npos) << message;
    }
    EXPECT_EQ(store().objects().totalObjectCount(), 0);
}

TEST(GeoJsonImportProgress, ProgressCancellationAndConstructorErrors) {
    const StoreFilesGuard files("import_progress");
    const Configuration config = ConfigurationBuilder().objectBatchSize(100).build();
    StorePtr store = Store::open(files.utf8().c_str(), OpenMode::CreateNew, config);
    LayerRegistry& layers = store->layers();
    const LayerId cells = layers.declareSemantic("cells", layers.pixelLayer());
    const StageId imp = store->stageStateWriter().declare("imp", "");

    // Progress after each committed batch and at the end, in features.
    std::vector<std::string> features;
    for (int i = 0; i < 350; ++i) features.push_back(squareFeature(i * 20, 0));
    const GeoJsonFile file(collection(features));
    RecordingObserver observer;
    GeoJsonImporter importer(*store, cells, imp);
    const ImportResult result = importer.import(file.utf8().c_str(), &observer, CancellationToken());
    EXPECT_EQ(result.objectsImported(), 350);
    ASSERT_EQ(observer.calls.size(), 4u);
    const int64_t expected[] = {100, 200, 300, 350};
    for (size_t i = 0; i < 4; ++i) {
        EXPECT_EQ(observer.calls[i].stage, "imp");
        EXPECT_EQ(observer.calls[i].completed, expected[i]);
        EXPECT_EQ(observer.calls[i].total, 350);
        EXPECT_DOUBLE_EQ(observer.calls[i].fraction, static_cast<double>(expected[i]) / 350.0);
    }

    // Cancelled while feature 150 is imported: the pending objects are committed first,
    // stay attributed to the stage, and discardStage removes them.
    const LayerId cells2 = layers.declareSemantic("cells2", layers.pixelLayer());
    const StageId imp2 = store->stageStateWriter().declare("imp2", "");
    std::vector<std::string> marked;
    for (int i = 0; i < 300; ++i) {
        marked.push_back(squareFeature(i * 20, 0, i == 150 ? R"("measurements":{"m":"x"})" : ""));
    }
    const GeoJsonFile markedFile(collection(marked));
    {
        LogCapture log;
        CancellationSource source;
        log.sink().source = &source;
        log.sink().trigger = "feature 150 ";
        GeoJsonImporter cancelled(*store, cells2, imp2);
        EXPECT_THROW(cancelled.import(markedFile.utf8().c_str(), nullptr, source.token()), CancelledError);
    }
    EXPECT_EQ(countAll(store->objects(), ObjectQuery().stage(imp2)), 151);
    store->objects().discardStage(imp2);
    EXPECT_EQ(countAll(store->objects(), ObjectQuery().stage(imp2)), 0);

    // Cancelled before the import pass: nothing is written.
    {
        CancellationSource source;
        source.cancel();
        GeoJsonImporter cancelled(*store, cells2, imp2);
        EXPECT_THROW(cancelled.import(markedFile.utf8().c_str(), nullptr, source.token()), CancelledError);
    }
    EXPECT_EQ(store->objects().totalObjectCount(), 350);

    // Constructor errors.
    const LayerId sp = layers.declarePrimitive("sp");
    layers.dropPrimitive(sp);
    EXPECT_THROW(GeoJsonImporter(*store, LayerId(999), imp), InvalidArgumentError);
    EXPECT_THROW(GeoJsonImporter(*store, layers.pixelLayer(), imp), InvalidArgumentError);
    EXPECT_THROW(GeoJsonImporter(*store, sp, imp), InvalidArgumentError);
    EXPECT_THROW(GeoJsonImporter(*store, cells, StageId(999)), InvalidArgumentError);
    const LayerId sp2 = layers.declarePrimitive("sp2");
    EXPECT_NO_THROW(GeoJsonImporter(*store, sp2, imp));  // both layer kinds are accepted
    EXPECT_THROW(importer.import(nullptr), InvalidArgumentError);
    store->close();
    store = Store::open(files.utf8().c_str(), OpenMode::ReadOnly, config);
    EXPECT_THROW(GeoJsonImporter(*store, cells, imp), IoError);
    store->close();
}

TEST(GeoJsonImportRoi, RoiPolygonIsTheFirstSupportedFeature) {  // UR-IMG-5
    const std::string withHole =
        R"({"type":"Feature","geometry":{"type":"Polygon","coordinates":[)"
        R"([[0,0],[100,0],[100,100],[0,100],[0,0]],[[10,10],[20,10],[20,20],[10,20],[10,10]]]},"properties":{}})";
    const GeoJsonFile file(collection({pointFeature(), withHole, squareFeature(500, 500)}));
    const Polygon roi = importRoiPolygon(file.utf8().c_str());
    ASSERT_EQ(roi.ringCount(), 2);
    EXPECT_FALSE(roi.ringIsHole(0));
    EXPECT_TRUE(roi.ringIsHole(1));
    EXPECT_EQ(roi.boundingBox(), (Rect64{0, 0, 100, 100}));

    const GeoJsonFile points(collection({pointFeature(), pointFeature()}));
    try {
        importRoiPolygon(points.utf8().c_str());
        FAIL() << "a file of points yielded a polygon";
    } catch (const InvalidArgumentError& e) {
        EXPECT_NE(std::string(e.what()).find("holds no polygon"), std::string::npos) << e.what();
    }

    // An invalid first polygon is an error, not a reason to take the next one.
    const GeoJsonFile invalid(collection(
        {R"({"type":"Feature","geometry":{"type":"Polygon","coordinates":[[[0,0],[5,0],[0,0]]]},"properties":{}})",
         squareFeature(0, 0)}));
    try {
        importRoiPolygon(invalid.utf8().c_str());
        FAIL() << "an invalid first polygon was passed over";
    } catch (const InvalidArgumentError& e) {
        EXPECT_EQ(std::string(e.what()).find("holds no polygon"), std::string::npos) << e.what();
    }

    EXPECT_THROW(importRoiPolygon(uniqueTempPath("import_missing", ".geojson").u8string().c_str()), IoError);
    EXPECT_THROW(importRoiPolygon(nullptr), InvalidArgumentError);
}
