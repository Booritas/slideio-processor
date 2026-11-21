// Requirements: UR-RES-8, UR-RES-11, UR-RES-15, UR-RES-18, UR-SEG-6, UR-QA-3
#include "reporting/export_fixture.hpp"
#include "storage/store_fixture.hpp"

#include <slideio/processor/config/configuration.hpp>
#include <slideio/processor/core/control.hpp>
#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/geojson.hpp>
#include <slideio/processor/core/geometry.hpp>
#include <slideio/processor/core/object_record.hpp>
#include <slideio/processor/reporting/reporting.hpp>
#include <slideio/processor/storage/annotation_store.hpp>
#include <slideio/processor/storage/geojson_import.hpp>
#include <slideio/processor/storage/layer_registry.hpp>
#include <slideio/processor/storage/object_store.hpp>
#include <slideio/processor/storage/stage_state_writer.hpp>
#include <slideio/processor/storage/store.hpp>
#include <slideio/processor/storage/vocabulary_registry.hpp>

#include <gtest/gtest.h>
#include <rapidjson/document.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace slideio::processor;
using namespace slideio::processor::export_test;

namespace {

std::string idText(ObjectId id) {
    std::vector<char> buffer(static_cast<size_t>(id.toString(nullptr, 0)) + 1);
    id.toString(buffer.data(), static_cast<int32_t>(buffer.size()));
    return std::string(buffer.data());
}

// The ids of every object, in the query cursor's order.
std::vector<ObjectId> cursorOrder(Store& store, const ObjectQuery& query = ObjectQuery()) {
    std::vector<ObjectId> ids;
    CursorPtr cursor = store.objects().query(query);
    while (cursor->next()) ids.push_back(cursor->current().id());
    cursor->close();
    return ids;
}

// Every object of the store, in cursor order.
std::vector<ObjectRecord> allRecords(Store& store, const ObjectQuery& query = ObjectQuery()) {
    std::vector<ObjectRecord> records;
    CursorPtr cursor = store.objects().query(query);
    while (cursor->next()) records.push_back(cursor->current());
    cursor->close();
    return records;
}

// Exports with `schema` (none: the default) and returns the file's text.
std::string exportGeoJson(Store& store, const OutputPath& out, const ColumnSchema* schema,
                          const ObjectQuery& query = ObjectQuery()) {
    GeoJsonExporter exporter(out.utf8().c_str());
    if (schema != nullptr) exporter.schema(*schema);
    const ExportResult result = exporter.write(store, query);
    EXPECT_FALSE(result.wasCancelled());
    const std::string text = readText(out.path());
    EXPECT_EQ(result.bytesWritten(), static_cast<int64_t>(text.size()));
    EXPECT_FALSE(std::filesystem::exists(out.partial()));
    return text;
}

// Parses `text`; fails the test when it is not valid JSON.
void parse(const std::string& text, rapidjson::Document& doc) {
    doc.Parse(text.c_str(), text.size());
    ASSERT_FALSE(doc.HasParseError()) << "offset " << doc.GetErrorOffset();
    ASSERT_TRUE(doc.IsObject());
    ASSERT_TRUE(doc.HasMember("features"));
    ASSERT_TRUE(doc["features"].IsArray());
}

// The properties of the feature whose objectId is `id`, or null.
const rapidjson::Value* propertiesOf(const rapidjson::Document& doc, ObjectId id) {
    const std::string text = idText(id);
    for (const rapidjson::Value& feature : doc["features"].GetArray()) {
        const rapidjson::Value& properties = feature["properties"];
        if (text == properties["objectId"].GetString()) return &properties;
    }
    return nullptr;
}

// The keys of a feature's "measurements", in document order.
std::vector<std::string> measurementKeys(const rapidjson::Value& properties) {
    std::vector<std::string> keys;
    for (const auto& member : properties["measurements"].GetObject())
        keys.emplace_back(member.name.GetString());
    return keys;
}

// The names of a feature's "labels", in order.
std::vector<std::string> labelNames(const rapidjson::Value& properties) {
    std::vector<std::string> names;
    for (const rapidjson::Value& label : properties["labels"].GetArray())
        names.emplace_back(label["name"].GetString());
    return names;
}

// A polygon's rings, each as its sorted distinct points, the rings sorted: equal for two
// polygons with the same rings whatever their start points.
using Ring = std::vector<std::pair<int64_t, int64_t>>;
std::vector<Ring> canonicalRings(const Polygon& polygon) {
    std::vector<Ring> rings;
    for (int32_t r = 0; r < polygon.ringCount(); ++r) {
        std::vector<Point64> points(static_cast<size_t>(polygon.ringPointCount(r)));
        polygon.ringPoints(r, points.data(), static_cast<int64_t>(points.size()));
        Ring ring;
        for (const Point64& p : points) ring.emplace_back(p.x, p.y);
        std::sort(ring.begin(), ring.end());
        ring.erase(std::unique(ring.begin(), ring.end()), ring.end());
        rings.push_back(ring);
    }
    std::sort(rings.begin(), rings.end());
    return rings;
}

// The shared export store (export_fixture.hpp), exported to a GeoJSON file.
class GeoJsonExport : public ExportStoreTest {
protected:
    std::string exportText(const ColumnSchema* schema = nullptr) {
        return exportGeoJson(store(), out, schema);
    }
    OutputPath out{"export", ".geojson"};
};

} // namespace

TEST_F(GeoJsonExport, WritesAFeatureCollection) {
    rapidjson::Document doc;
    parse(exportText(), doc);
    ASSERT_FALSE(HasFatalFailure());
    EXPECT_STREQ(doc["type"].GetString(), "FeatureCollection");
    ASSERT_TRUE(doc.HasMember("provenance"));
    ASSERT_TRUE(doc["provenance"].IsObject());
    EXPECT_STREQ(doc["provenance"]["slideId"].GetString(), "slide-7");

    // One feature per object, in cursor order.
    const std::vector<ObjectId> order = cursorOrder(store());
    const auto features = doc["features"].GetArray();
    ASSERT_EQ(features.Size(), order.size());
    for (rapidjson::SizeType i = 0; i < features.Size(); ++i) {
        const rapidjson::Value& properties = features[i]["properties"];
        EXPECT_EQ(properties["objectId"].GetString(), idText(order[i]));
        EXPECT_STREQ(properties["objectType"].GetString(), "detection");
        ASSERT_TRUE(properties["labels"].IsArray());
        if (properties["labels"].Size() > 0) {
            EXPECT_STREQ(properties["classification"]["name"].GetString(),
                         properties["labels"][0]["name"].GetString());
        } else {
            EXPECT_FALSE(properties.HasMember("classification"));
        }
    }

    // The region: its layer, its label, and a Polygon with the hole as a second ring.
    const rapidjson::Value* region = propertiesOf(doc, regionId);
    ASSERT_NE(region, nullptr);
    EXPECT_STREQ((*region)["layer"].GetString(), "regions");
    for (const rapidjson::Value& feature : features) {
        if (idText(regionId) != feature["properties"]["objectId"].GetString()) continue;
        EXPECT_STREQ(feature["geometry"]["type"].GetString(), "Polygon");
        EXPECT_EQ(feature["geometry"]["coordinates"].Size(), 2u);
    }

    // Cell 1 has no labels; cell 2 has the whole effective set, in store order, with the
    // local flags.
    const rapidjson::Value* cell1 = propertiesOf(doc, cell1Id);
    ASSERT_NE(cell1, nullptr);
    EXPECT_STREQ((*cell1)["layer"].GetString(), "cells");
    EXPECT_TRUE(labelNames(*cell1).empty());

    const rapidjson::Value* cell2 = propertiesOf(doc, cell2Id);
    ASSERT_NE(cell2, nullptr);
    AnnotationStore& annotations = store().annotations();
    const int32_t count = annotations.labelCount(cell2Id);
    ASSERT_EQ(count, 2);
    ASSERT_EQ((*cell2)["labels"].Size(), 2u);
    for (int32_t i = 0; i < count; ++i) {
        const LabelId label = annotations.labelAt(cell2Id, i);
        const rapidjson::Value& entry = (*cell2)["labels"][static_cast<rapidjson::SizeType>(i)];
        EXPECT_STREQ(entry["name"].GetString(), store().vocabulary().name(label));
        EXPECT_EQ(entry["workspaceLocal"].GetBool(), label == myLocal);
    }
}

TEST_F(GeoJsonExport, DefaultSchemaKeepsEveryMeasurement) {
    rapidjson::Document doc;
    parse(exportText(), doc);
    ASSERT_FALSE(HasFatalFailure());
    const rapidjson::Value* cell1 = propertiesOf(doc, cell1Id);
    ASSERT_NE(cell1, nullptr);
    EXPECT_EQ(measurementKeys(*cell1),
              (std::vector<std::string>{"area", "area.um2", "perimeter"}));
    EXPECT_DOUBLE_EQ((*cell1)["measurements"]["area"].GetDouble(), 100.0);
    EXPECT_DOUBLE_EQ((*cell1)["measurements"]["area.um2"].GetDouble(), 25.0);
    EXPECT_DOUBLE_EQ((*cell1)["measurements"]["perimeter"].GetDouble(), 40.0);

    // The region has no perimeter, so it has no such key.
    const rapidjson::Value* region = propertiesOf(doc, regionId);
    ASSERT_NE(region, nullptr);
    EXPECT_EQ(measurementKeys(*region), (std::vector<std::string>{"area", "area.um2"}));
    EXPECT_DOUBLE_EQ((*region)["measurements"]["area"].GetDouble(), 9900.0);
    EXPECT_DOUBLE_EQ((*region)["measurements"]["area.um2"].GetDouble(), 2475.0);
}

TEST_F(GeoJsonExport, SchemaSelectsMeasurementsAndUnits) {
    ColumnSchema calibrated;
    calibrated.measurement("area").units(UnitPolicy::Calibrated);
    rapidjson::Document doc;
    parse(exportText(&calibrated), doc);
    ASSERT_FALSE(HasFatalFailure());
    for (const rapidjson::Value& feature : doc["features"].GetArray())
        EXPECT_EQ(measurementKeys(feature["properties"]),
                  (std::vector<std::string>{"area.um2"}));

    ColumnSchema pixels;
    pixels.measurement("area").units(UnitPolicy::PixelsOnly);
    rapidjson::Document pixelDoc;
    parse(exportText(&pixels), pixelDoc);
    ASSERT_FALSE(HasFatalFailure());
    for (const rapidjson::Value& feature : pixelDoc["features"].GetArray())
        EXPECT_EQ(measurementKeys(feature["properties"]), (std::vector<std::string>{"area"}));
}

TEST_F(GeoJsonExport, ProducerOnlyLabels) {
    ColumnSchema schema;
    schema.allMeasurements().labelPolicy(LabelPolicy::ProducerOnly);
    rapidjson::Document doc;
    parse(exportText(&schema), doc);
    ASSERT_FALSE(HasFatalFailure());
    const rapidjson::Value* cell2 = propertiesOf(doc, cell2Id);
    ASSERT_NE(cell2, nullptr);
    EXPECT_EQ(labelNames(*cell2),
              (std::vector<std::string>{store().vocabulary().name(tumour)}));
    EXPECT_FALSE((*cell2)["labels"][0]["workspaceLocal"].GetBool());
    EXPECT_STREQ((*cell2)["classification"]["name"].GetString(),
                 store().vocabulary().name(tumour));

    const rapidjson::Value* cell1 = propertiesOf(doc, cell1Id);
    ASSERT_NE(cell1, nullptr);
    EXPECT_TRUE(labelNames(*cell1).empty());
    EXPECT_FALSE(cell1->HasMember("classification"));
}

TEST_F(GeoJsonExport, ParentIdWhenRequested) {
    ColumnSchema schema;
    schema.allMeasurements().includeParent(true);
    rapidjson::Document doc;
    parse(exportText(&schema), doc);
    ASSERT_FALSE(HasFatalFailure());
    for (const ObjectId child : {cell1Id, cell2Id}) {
        const rapidjson::Value* properties = propertiesOf(doc, child);
        ASSERT_NE(properties, nullptr);
        ASSERT_TRUE(properties->HasMember("parentId"));
        EXPECT_EQ((*properties)["parentId"].GetString(), idText(regionId));
    }
    const rapidjson::Value* region = propertiesOf(doc, regionId);
    ASSERT_NE(region, nullptr);
    EXPECT_FALSE(region->HasMember("parentId"));

    rapidjson::Document without;
    parse(exportText(), without);
    ASSERT_FALSE(HasFatalFailure());
    for (const rapidjson::Value& feature : without["features"].GetArray())
        EXPECT_FALSE(feature["properties"].HasMember("parentId"));
}

TEST_F(GeoJsonExport, NonFiniteMeasurementIsOmitted) {
    ObjectRecord record;
    record.setLayer(cells);
    record.setGeometry(square(300, 0, 4));
    record.measurements().set("m_pinf", std::numeric_limits<double>::infinity(),
                              MeasurementUnit::Pixels);
    record.measurements().set("size", 16.0, MeasurementUnit::Pixels);
    commitOne(store(), "more", record);

    rapidjson::Document doc;
    parse(exportGeoJson(store(), out, nullptr), doc);
    ASSERT_FALSE(HasFatalFailure());
    const rapidjson::Value* properties = propertiesOf(doc, record.id());
    ASSERT_NE(properties, nullptr);
    EXPECT_FALSE((*properties)["measurements"].HasMember("m_pinf"));
    ASSERT_TRUE((*properties)["measurements"].HasMember("size"));
    EXPECT_DOUBLE_EQ((*properties)["measurements"]["size"].GetDouble(), 16.0);
}

TEST_F(GeoJsonExport, RoundTripsThroughTheImporter) {
    const std::vector<ObjectRecord> originals = allRecords(store(), ObjectQuery().layer(cells));
    ASSERT_EQ(originals.size(), 2u);
    exportGeoJson(store(), out, nullptr, ObjectQuery().layer(cells));

    StoreFilesGuard files("imported");
    Configuration targetConfig;
    StorePtr target = Store::open(files.utf8().c_str(), OpenMode::CreateNew, targetConfig);
    const LayerId layer = target->layers().declareSemantic("imported", target->layers().pixelLayer());
    const StageId stage = target->stageStateWriter().declare("import", "");
    target->vocabulary().setImportPolicy(ImportPolicy::Permissive);
    const ImportResult result = GeoJsonImporter(*target, layer, stage).import(out.utf8().c_str());
    target->stageStateWriter().setStatus(stage, StageStatus::Complete);
    EXPECT_EQ(result.objectsImported(), static_cast<int64_t>(originals.size()));

    const std::vector<ObjectRecord> imported = allRecords(*target);
    ASSERT_EQ(imported.size(), originals.size());
    for (const ObjectRecord& original : originals) {
        const auto match = std::find_if(imported.begin(), imported.end(), [&](const ObjectRecord& r) {
            return canonicalRings(r.geometry()) == canonicalRings(original.geometry());
        });
        ASSERT_NE(match, imported.end());

        // The classification is the first effective label.
        AnnotationStore& annotations = store().annotations();
        if (annotations.labelCount(original.id()) == 0) {
            EXPECT_FALSE(match->hasLabel());
        } else {
            ASSERT_TRUE(match->hasLabel());
            EXPECT_STREQ(target->vocabulary().name(match->label()),
                         store().vocabulary().name(annotations.labelAt(original.id(), 0)));
        }

        // Every measurement, by name.
        const MeasurementSet& want = original.measurements();
        ASSERT_EQ(match->measurements().count(), want.count());
        for (int32_t i = 0; i < want.count(); ++i) {
            const Measurement m = want.at(i);
            Measurement got;
            ASSERT_TRUE(match->measurements().find(m.name(), got)) << m.name();
            EXPECT_EQ(got.value(), m.value()) << m.name();
        }
    }
    target->close();
}

TEST_F(GeoJsonExport, NamesNeedingEscapesRoundTrip) {
    const char* const labelName = "a\"b\\c";
    const char* const layerName = u8"Zellkerne ü";
    const LayerId odd = store().layers().declareSemantic(layerName, regions);
    const LabelId label = store().vocabulary().addLocal(labelName);
    ObjectRecord record;
    record.setLayer(odd);
    record.setGeometry(square(300, 0, 4));
    record.setLabel(label);
    commitOne(store(), "more", record);

    const std::string text = exportGeoJson(store(), out, nullptr, ObjectQuery().layer(odd));
    rapidjson::Document doc;
    parse(text, doc);
    ASSERT_FALSE(HasFatalFailure());
    const rapidjson::Value* properties = propertiesOf(doc, record.id());
    ASSERT_NE(properties, nullptr);
    EXPECT_STREQ((*properties)["layer"].GetString(), layerName);
    EXPECT_EQ(labelNames(*properties), (std::vector<std::string>{labelName}));
    EXPECT_TRUE((*properties)["labels"][0]["workspaceLocal"].GetBool());

    // The library's own reader sees the same names.
    GeoJsonReader reader(out.utf8().c_str());
    ASSERT_TRUE(reader.next());
    EXPECT_STREQ(reader.layerName(), layerName);
    EXPECT_STREQ(reader.classification(), labelName);
    EXPECT_FALSE(reader.next());

    // And the importer registers the label under its own name.
    StoreFilesGuard files("imported");
    Configuration targetConfig;
    StorePtr target = Store::open(files.utf8().c_str(), OpenMode::CreateNew, targetConfig);
    const LayerId layer = target->layers().declareSemantic("imported", target->layers().pixelLayer());
    const StageId stage = target->stageStateWriter().declare("import", "");
    target->vocabulary().setImportPolicy(ImportPolicy::Permissive);
    GeoJsonImporter(*target, layer, stage).import(out.utf8().c_str());
    target->stageStateWriter().setStatus(stage, StageStatus::Complete);
    const std::vector<ObjectRecord> imported = allRecords(*target);
    ASSERT_EQ(imported.size(), 1u);
    ASSERT_TRUE(imported[0].hasLabel());
    EXPECT_STREQ(target->vocabulary().name(imported[0].label()), labelName);
    target->close();
}

TEST_F(GeoJsonExport, ExportIsDeterministic) {
    ColumnSchema schema;
    schema.allMeasurements().units(UnitPolicy::BothColumns).includeParent(true);
    const std::string first = exportText(&schema);
    const std::string second = exportText(&schema);
    EXPECT_FALSE(first.empty());
    EXPECT_EQ(first, second);
}

TEST_F(GeoJsonExport, CancelledExportLeavesNoFile) {
    CancellationSource source;
    source.cancel();
    RecordingObserver observer;
    const ExportResult result = GeoJsonExporter(out.utf8().c_str())
                                    .write(store(), ObjectQuery(), &observer, source.token());
    EXPECT_TRUE(result.wasCancelled());
    EXPECT_EQ(result.objectsWritten(), 0);
    EXPECT_EQ(result.bytesWritten(), 0);
    EXPECT_FALSE(std::filesystem::exists(out.path()));
    EXPECT_FALSE(std::filesystem::exists(out.partial()));
}

TEST_F(GeoJsonExport, CancelledExportKeepsAnExistingFile) {
    writeText(out.path(), "previous export\n");
    CancellationSource source;
    source.cancel();
    const ExportResult result = GeoJsonExporter(out.utf8().c_str())
                                    .write(store(), ObjectQuery(), nullptr, source.token());
    EXPECT_TRUE(result.wasCancelled());
    EXPECT_EQ(readText(out.path()), "previous export\n");
    EXPECT_FALSE(std::filesystem::exists(out.partial()));
}

TEST_F(GeoJsonExport, ReportsProgress) {
    StoreFilesGuard files;
    Configuration bigConfig;
    StorePtr bigStore = Store::open(files.utf8().c_str(), OpenMode::CreateNew, bigConfig);
    const LayerId layer = bigStore->layers().declareSemantic("cells", bigStore->layers().pixelLayer());
    const StageId stage = bigStore->stageStateWriter().declare("seg", "{}");
    ObjectWriterPtr writer = bigStore->objects().writerForStage(stage);
    for (int64_t i = 0; i < 2500; ++i) {
        ObjectRecord record;
        record.setLayer(layer);
        record.setGeometry(square(3 * (i % 100), 3 * (i / 100), 2));
        writer->emit(record);
    }
    writer->flush();
    bigStore->stageStateWriter().setStatus(stage, StageStatus::Complete);

    OutputPath progressOut("export", ".geojson");
    RecordingObserver observer;
    const ExportResult result = GeoJsonExporter(progressOut.utf8().c_str())
                                    .write(*bigStore, ObjectQuery(), &observer, CancellationToken());
    EXPECT_EQ(result.objectsWritten(), 2500);
    ASSERT_EQ(observer.calls.size(), 3u);
    const int64_t expected[] = {1000, 2000, 2500};
    for (size_t i = 0; i < 3; ++i) {
        EXPECT_EQ(observer.calls[i].stage, "export");
        EXPECT_EQ(observer.calls[i].completed, expected[i]);
        EXPECT_EQ(observer.calls[i].total, 2500);
        EXPECT_DOUBLE_EQ(observer.calls[i].fraction, static_cast<double>(expected[i]) / 2500.0);
    }

    // Cancelled from the observer after 1000 objects: the next check stops the export.
    OutputPath cancelledOut("export", ".geojson");
    CancellationSource source;
    CancellingObserver canceller(source, 1000);
    const ExportResult cancelled = GeoJsonExporter(cancelledOut.utf8().c_str())
                                       .write(*bigStore, ObjectQuery(), &canceller, source.token());
    EXPECT_TRUE(cancelled.wasCancelled());
    EXPECT_EQ(cancelled.objectsWritten(), 1000);
    EXPECT_FALSE(std::filesystem::exists(cancelledOut.path()));
    EXPECT_FALSE(std::filesystem::exists(cancelledOut.partial()));
    bigStore->close();
}

TEST_F(GeoJsonExport, BytesWrittenIsTheFileSize) {
    const ExportResult result = GeoJsonExporter(out.utf8().c_str()).write(store(), ObjectQuery());
    EXPECT_EQ(result.objectsWritten(), 3);
    EXPECT_GT(result.bytesWritten(), 0);
    EXPECT_EQ(result.bytesWritten(), static_cast<int64_t>(std::filesystem::file_size(out.path())));
    EXPECT_FALSE(std::filesystem::exists(out.partial()));
}

TEST_F(GeoJsonExport, LaterStageMeasurementsAreKeys) {
    assertFraction(store(), "ki67", {{cell1Id, 0.25}});
    rapidjson::Document doc;
    parse(exportText(), doc);
    ASSERT_FALSE(HasFatalFailure());
    const rapidjson::Value* cell1 = propertiesOf(doc, cell1Id);
    ASSERT_NE(cell1, nullptr);
    ASSERT_TRUE((*cell1)["measurements"].HasMember("ki67/positive_fraction"));
    EXPECT_DOUBLE_EQ((*cell1)["measurements"]["ki67/positive_fraction"].GetDouble(), 0.25);
    EXPECT_EQ(measurementKeys(*cell1).back(), "ki67/positive_fraction");

    const rapidjson::Value* cell2 = propertiesOf(doc, cell2Id);
    ASSERT_NE(cell2, nullptr);
    EXPECT_FALSE((*cell2)["measurements"].HasMember("ki67/positive_fraction"));
}

TEST_F(GeoJsonExport, RefusesAnEmptyPath) {
    EXPECT_THROW(GeoJsonExporter(nullptr), InvalidArgumentError);
    EXPECT_THROW(GeoJsonExporter(""), InvalidArgumentError);
}

TEST_F(GeoJsonExport, UnwritablePathThrowsAndLeavesNothing) {
    const std::filesystem::path missing =
        out.path().parent_path() / "no-such-dir-geojson" / "x.geojson";
    std::filesystem::path partial = missing;
    partial += ".partial";
    GeoJsonExporter exporter(missing.u8string().c_str());
    try {
        exporter.write(store(), ObjectQuery());
        ADD_FAILURE() << "expected IoError";
    } catch (const IoError& e) {
        // The writer's own message names the path; the exporter's does not.
        EXPECT_EQ(std::string(e.what()).find("no-such-dir-geojson"), std::string::npos)
            << e.what();
    }
    EXPECT_FALSE(std::filesystem::exists(missing));
    EXPECT_FALSE(std::filesystem::exists(partial));
}

// GeoJSON measurements are a nested object, so a name like a fixed CSV column is kept.
TEST_F(GeoJsonExport, FixedColumnNamedMeasurementIsKept) {
    ObjectRecord record;
    record.setLayer(cells);
    record.setGeometry(square(300, 0, 4));
    record.measurements().set("labels", 1.0, MeasurementUnit::Dimensionless);
    record.measurements().set("layer", 2.0, MeasurementUnit::Dimensionless);
    commitOne(store(), "more", record);

    rapidjson::Document doc;
    parse(exportGeoJson(store(), out, nullptr), doc);
    ASSERT_FALSE(HasFatalFailure());
    const rapidjson::Value* properties = propertiesOf(doc, record.id());
    ASSERT_NE(properties, nullptr);
    EXPECT_EQ(measurementKeys(*properties), (std::vector<std::string>{"labels", "layer"}));
    EXPECT_DOUBLE_EQ((*properties)["measurements"]["labels"].GetDouble(), 1.0);
    EXPECT_DOUBLE_EQ((*properties)["measurements"]["layer"].GetDouble(), 2.0);
    // The feature's own "layer" property is untouched.
    EXPECT_STREQ((*properties)["layer"].GetString(), "cells");
}

// A twin with no base in the store (an importer can write one) is still exported.
TEST_F(GeoJsonExport, RowTwinWithoutBaseIsKept) {
    ObjectRecord record;
    record.setLayer(cells);
    record.setGeometry(square(300, 0, 4));
    record.measurements().set("len.um", 5.0, MeasurementUnit::Micrometres);
    commitOne(store(), "more", record);

    rapidjson::Document doc;
    parse(exportGeoJson(store(), out, nullptr), doc);
    ASSERT_FALSE(HasFatalFailure());
    const rapidjson::Value* properties = propertiesOf(doc, record.id());
    ASSERT_NE(properties, nullptr);
    EXPECT_EQ(measurementKeys(*properties), (std::vector<std::string>{"len.um"}));
    EXPECT_DOUBLE_EQ((*properties)["measurements"]["len.um"].GetDouble(), 5.0);
}

TEST_F(GeoJsonExport, ClashingNamesAreRefusedBeforeTheFileIsCreated) {
    ObjectRecord record;
    record.setLayer(cells);
    record.setGeometry(square(300, 0, 4));
    record.measurements().set("ki67/positive_fraction", 1.0, MeasurementUnit::Dimensionless);
    commitOne(store(), "more", record);
    assertFraction(store(), "ki67", {{cell1Id, 0.25}});

    EXPECT_THROW(GeoJsonExporter(out.utf8().c_str()).write(store(), ObjectQuery()),
                 InvalidArgumentError);
    EXPECT_FALSE(std::filesystem::exists(out.path()));
    EXPECT_FALSE(std::filesystem::exists(out.partial()));
}

TEST_F(GeoJsonExport, InvisibleAssertionHasNoKey) {
    assertFraction(store(), "late", {{cell1Id, 0.5}}, /*complete=*/false);
    rapidjson::Document doc;
    parse(exportText(), doc);
    ASSERT_FALSE(HasFatalFailure());
    for (const rapidjson::Value& feature : doc["features"].GetArray())
        EXPECT_FALSE(feature["properties"]["measurements"].HasMember("late/positive_fraction"));
}

namespace {
// Throws from the call that reports every object done.
class ThrowingAtTheEndObserver : public ProgressObserver {
public:
    void onProgress(const char*, double, int64_t completed, int64_t total) override {
        if (completed == total) throw std::runtime_error("observer failed");
    }
};
} // namespace

TEST_F(GeoJsonExport, ObserverThrowingAtTheEndLeavesAnExistingFile) {
    writeText(out.path(), "previous export\n");
    ThrowingAtTheEndObserver observer;
    EXPECT_THROW(GeoJsonExporter(out.utf8().c_str())
                     .write(store(), ObjectQuery(), &observer, CancellationToken()),
                 std::runtime_error);
    EXPECT_EQ(readText(out.path()), "previous export\n");
    EXPECT_FALSE(std::filesystem::exists(out.partial()));
}
