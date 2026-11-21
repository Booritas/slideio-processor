// Requirements: UR-RES-9, UR-RES-11, UR-RES-15, UR-RES-18, UR-SEG-6, UR-QA-3
#include "common/temp_path.hpp"
#include "storage/store_fixture.hpp"

#include "reporting/detail/csv_format.hpp"

#include <slideio/processor/config/configuration.hpp>
#include <slideio/processor/core/control.hpp>
#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/geometry.hpp>
#include <slideio/processor/core/object_record.hpp>
#include <slideio/processor/core/provenance.hpp>
#include <slideio/processor/reporting/reporting.hpp>
#include <slideio/processor/storage/annotation_store.hpp>
#include <slideio/processor/storage/edge_store.hpp>
#include <slideio/processor/storage/layer_registry.hpp>
#include <slideio/processor/storage/object_store.hpp>
#include <slideio/processor/storage/stage_state_writer.hpp>
#include <slideio/processor/storage/store.hpp>
#include <slideio/processor/storage/vocabulary_registry.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

using namespace slideio::processor;

namespace {

using Row = std::vector<std::string>;

// A CSV output path; the file and its ".partial" sibling are removed on destruction.
class OutputPath {
public:
    explicit OutputPath(const char* stem = "export") : path_(uniqueTempPath(stem, ".csv")) {}
    OutputPath(const OutputPath&) = delete;
    OutputPath& operator=(const OutputPath&) = delete;
    ~OutputPath() {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
        std::filesystem::remove(partial(), ec);
    }
    const std::filesystem::path& path() const { return path_; }
    std::filesystem::path partial() const {
        std::filesystem::path p = path_;
        p += ".partial";
        return p;
    }
    std::string utf8() const { return path_.u8string(); }

private:
    std::filesystem::path path_;
};

std::string readText(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void writeText(const std::filesystem::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary);
    out << text;
}

// A minimal RFC 4180 reader: records end in "\n"; a quoted field may hold the
// delimiter, "\n" and doubled quotes. As pandas.read_csv(comment='#') does, an
// unquoted '#' starts a comment: at a record start the whole line is skipped, and
// mid-record the rest of the line is dropped. A quoted '#' stays data.
std::vector<Row> parseCsv(const std::string& text, char delimiter = ',') {
    std::vector<Row> rows;
    size_t i = 0;
    while (i < text.size()) {
        if (text[i] == '#') {
            i = text.find('\n', i);
            i = i == std::string::npos ? text.size() : i + 1;
            continue;
        }
        Row row;
        std::string field;
        bool quoted = false;
        for (; i < text.size(); ++i) {
            const char c = text[i];
            if (quoted) {
                if (c == '"') {
                    if (i + 1 < text.size() && text[i + 1] == '"') {
                        field += '"';
                        ++i;
                    } else {
                        quoted = false;
                    }
                } else {
                    field += c;
                }
            } else if (c == '"') {
                quoted = true;
            } else if (c == '#') {
                // The rest of the line is a comment; the record ends here.
                i = text.find('\n', i);
                i = i == std::string::npos ? text.size() : i + 1;
                break;
            } else if (c == delimiter) {
                row.push_back(field);
                field.clear();
            } else if (c == '\n') {
                ++i;
                break;
            } else {
                field += c;
            }
        }
        row.push_back(field);
        rows.push_back(row);
    }
    return rows;
}

std::vector<std::string> lines(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) out.push_back(line);
    return out;
}

// Splits a labels cell on unescaped ';' and undoes the "\\" and "\;" escapes.
std::vector<std::string> splitLabels(const std::string& cell) {
    std::vector<std::string> out;
    if (cell.empty()) return out;
    std::string current;
    for (size_t i = 0; i < cell.size(); ++i) {
        if (cell[i] == '\\' && i + 1 < cell.size()) {
            current += cell[++i];
        } else if (cell[i] == ';') {
            out.push_back(current);
            current.clear();
        } else {
            current += cell[i];
        }
    }
    out.push_back(current);
    return out;
}

Polygon square(int64_t x, int64_t y, int64_t side) {
    const Point64 points[] = {{x, y}, {x + side, y}, {x + side, y + side}, {x, y + side}};
    Polygon polygon;
    polygon.addRing(points, 4, false);
    return polygon;
}

std::string decimal(int64_t value) { return std::to_string(value); }

// ColumnSchema::toHeaderLine() as a string.
std::string header(const ColumnSchema& schema) {
    const int32_t n = schema.toHeaderLine(nullptr, 0);
    std::vector<char> buffer(static_cast<size_t>(n) + 1);
    schema.toHeaderLine(buffer.data(), static_cast<int32_t>(buffer.size()));
    return std::string(buffer.data());
}

class RecordingObserver : public ProgressObserver {
public:
    struct Call {
        std::string stage;
        double fraction;
        int64_t completed;
        int64_t total;
    };
    void onProgress(const char* stageName, double fraction, int64_t completed,
                    int64_t total) override {
        calls.push_back({stageName, fraction, completed, total});
    }
    std::vector<Call> calls;
};

// A region (tumour, with a hole, the root) on the pixel layer and two cells above it.
// Cell 1 has no label and a parent; cell 2 has the row label tumour, an asserted local
// label myLocal, and a parent. Area has calibrated twins; perimeter does not.
class CsvExportTest : public StoreFixture {
protected:
    void SetUp() override {
        StoreFixture::SetUp();
        Provenance provenance = store().provenance();
        provenance.setSlideId("slide-7");
        provenance.setSceneIndex(2);
        store().setProvenance(provenance);

        LayerRegistry& layers = store().layers();
        regions = layers.declareSemantic("regions", layers.pixelLayer());
        cells = layers.declareSemantic("cells", regions);
        seg = store().stageStateWriter().declare("seg", "{\n\"k\":1\r\n}");
        classify = store().stageStateWriter().declare("classify", "{}");
        ASSERT_TRUE(store().vocabulary().find("tumour", tumour));
        myLocal = store().vocabulary().addLocal("myLocal");

        Polygon withHole = square(0, 0, 100);
        const Point64 hole[] = {{10, 10}, {20, 10}, {20, 20}, {10, 20}};
        withHole.addRing(hole, 4, true);
        ObjectRecord region;
        region.setLayer(regions);
        region.setGeometry(withHole);
        region.setLabel(tumour);
        region.measurements().set("area", 9900.0, MeasurementUnit::SquarePixels);
        region.measurements().set("area.um2", 2475.0, MeasurementUnit::SquareMicrometres);

        ObjectRecord cell1;
        cell1.setLayer(cells);
        cell1.setGeometry(square(30, 30, 10));
        cell1.measurements().set("area", 100.0, MeasurementUnit::SquarePixels);
        cell1.measurements().set("area.um2", 25.0, MeasurementUnit::SquareMicrometres);
        cell1.measurements().set("perimeter", 40.0, MeasurementUnit::Pixels);

        ObjectRecord cell2;
        cell2.setLayer(cells);
        cell2.setGeometry(square(50, 50, 10));
        cell2.setLabel(tumour);
        cell2.measurements().set("area", 100.0, MeasurementUnit::SquarePixels);
        cell2.measurements().set("area.um2", 25.0, MeasurementUnit::SquareMicrometres);
        cell2.measurements().set("perimeter", 0.1, MeasurementUnit::Pixels);

        ObjectWriterPtr writer = store().objects().writerForStage(seg);
        for (ObjectRecord* r : {&region, &cell1, &cell2}) writer->emit(*r);
        writer->flush();
        regionId = region.id();
        cell1Id = cell1.id();
        cell2Id = cell2.id();

        EdgeWriterPtr edges = store().edges().writerForStage(seg);
        edges->assertParent(cell1Id, regionId);
        edges->assertParent(cell2Id, regionId);
        edges->flush();
        store().stageStateWriter().setStatus(seg, StageStatus::Complete);

        AnnotationWriterPtr labels = store().annotations().writerForStage(classify);
        labels->assertLabel(cell2Id, myLocal);
        labels->flush();
        store().stageStateWriter().setStatus(classify, StageStatus::Complete);
    }

    // Exports with `schema` and returns the file's text.
    std::string exportText(const ColumnSchema& schema, char delimiter = ',',
                           bool provenance = true) {
        CsvWriter writer(out.utf8().c_str());
        writer.schema(schema).delimiter(delimiter).provenanceHeader(provenance);
        const ExportResult result = writer.write(store(), ObjectQuery());
        EXPECT_FALSE(result.wasCancelled());
        EXPECT_EQ(result.objectsWritten(), 3);
        const std::string text = readText(out.path());
        EXPECT_EQ(result.bytesWritten(), static_cast<int64_t>(text.size()));
        EXPECT_FALSE(std::filesystem::exists(out.partial()));
        return text;
    }

    std::string idCells(ObjectId id) const {
        return decimal(id.layer().value()) + "," + decimal(id.anchorPacked());
    }

    OutputPath out;
    LayerId regions, cells;
    StageId seg, classify;
    LabelId tumour, myLocal;
    ObjectId regionId, cell1Id, cell2Id;
};

} // namespace

TEST_F(CsvExportTest, WritesHeaderAndRows) {
    ColumnSchema schema;
    schema.measurement("area").measurement("perimeter");
    const Provenance p = store().provenance();
    const std::string expected =
        std::string("# slideio-processor export\n") + "# slide_id: slide-7\n" +
        "# scene_index: 2\n" + "# library_version: " + p.libraryVersion() + "\n" +
        "# created_at_unix_ms: " + decimal(p.createdAtUnixMs()) + "\n" +
        "# stage: seg {\\n\"k\":1\\r\\n}\n" + "# stage: classify {}\n" +
        "layer_id,anchor_id,layer,stage,labels,labels_local,bbox_x,bbox_y,bbox_width,"
        "bbox_height,area.um2,perimeter\n" +
        idCells(regionId) + ",regions,seg,tumour,0,0,0,100,100,2475,\n" + idCells(cell1Id) +
        ",cells,seg,,,30,30,10,10,25,40\n" + idCells(cell2Id) +
        ",cells,seg,tumour;myLocal,0;1,50,50,10,10,25,0.10000000000000001\n";
    EXPECT_EQ(exportText(schema), expected);
}

TEST_F(CsvExportTest, EffectiveLabelsAndLocalFlags) {
    const std::vector<Row> rows = parseCsv(exportText(ColumnSchema()));
    ASSERT_EQ(rows.size(), 4u);
    EXPECT_EQ(rows[0][4], "labels");
    EXPECT_EQ(rows[0][5], "labels_local");
    EXPECT_EQ(rows[3][4], "tumour;myLocal");
    EXPECT_EQ(rows[3][5], "0;1");
    EXPECT_EQ(rows[2][4], "");
    EXPECT_EQ(rows[2][5], "");

    ColumnSchema producer;
    producer.labelPolicy(LabelPolicy::ProducerOnly);
    const std::vector<Row> only = parseCsv(exportText(producer));
    ASSERT_EQ(only.size(), 4u);
    EXPECT_EQ(only[0][4], "label");
    EXPECT_EQ(only[0][5], "label_local");
    EXPECT_EQ(only[3][4], "tumour");
    EXPECT_EQ(only[3][5], "0");
    EXPECT_EQ(only[2][4], "");
}

TEST_F(CsvExportTest, LabelNamesWithDelimitersRoundTrip) {
    const char* awkward = "a;b\\c\"d,e";
    const LabelId label = store().vocabulary().addLocal(awkward);
    const StageId later = store().stageStateWriter().declare("later", "{}");
    AnnotationWriterPtr writer = store().annotations().writerForStage(later);
    writer->assertLabel(cell1Id, label);
    writer->flush();
    store().stageStateWriter().setStatus(later, StageStatus::Complete);

    const std::string text = exportText(ColumnSchema());
    EXPECT_NE(text.find(",\"a\\;b\\\\c\"\"d,e\",1,"), std::string::npos) << text;
    const std::vector<Row> rows = parseCsv(text);
    ASSERT_EQ(rows.size(), 4u);
    ASSERT_EQ(rows[2].size(), 10u);
    const std::vector<std::string> names = splitLabels(rows[2][4]);
    ASSERT_EQ(names.size(), 1u);
    EXPECT_EQ(names[0], awkward);
    EXPECT_EQ(rows[2][5], "1");
}

TEST_F(CsvExportTest, ParentColumns) {
    ColumnSchema schema;
    schema.includeParent(true);
    const std::vector<Row> rows = parseCsv(exportText(schema));
    ASSERT_EQ(rows.size(), 4u);
    EXPECT_EQ(rows[0][10], "parent_layer_id");
    EXPECT_EQ(rows[0][11], "parent_anchor_id");
    EXPECT_EQ(rows[1][10], "");
    EXPECT_EQ(rows[1][11], "");
    EXPECT_EQ(rows[2][10], decimal(regionId.layer().value()));
    EXPECT_EQ(rows[2][11], decimal(regionId.anchorPacked()));
    EXPECT_EQ(rows[3][10], decimal(regionId.layer().value()));

    ColumnSchema packed;
    packed.idFormat(IdFormat::PackedString).includeParent(true);
    const std::vector<Row> prow = parseCsv(exportText(packed));
    char text[64];
    regionId.toString(text, sizeof(text));
    EXPECT_EQ(prow[0][9], "parent_id");
    EXPECT_EQ(prow[1][0], text);
    EXPECT_EQ(prow[1][9], "");
    EXPECT_EQ(prow[2][9], text);
}

TEST_F(CsvExportTest, GeometryWkt) {
    // A second outer ring, with a hole that lies inside it and not inside the first.
    Polygon two = square(200, 0, 10);
    const Point64 second[] = {{220, 0}, {240, 0}, {240, 20}, {220, 20}};
    two.addRing(second, 4, false);
    const Point64 hole[] = {{225, 5}, {230, 5}, {230, 10}, {225, 10}};
    two.addRing(hole, 4, true);
    ObjectRecord record;
    record.setLayer(cells);
    record.setGeometry(two);
    const StageId more = store().stageStateWriter().declare("more", "{}");
    ObjectWriterPtr writer = store().objects().writerForStage(more);
    writer->emit(record);
    writer->flush();
    store().stageStateWriter().setStatus(more, StageStatus::Complete);

    ColumnSchema schema;
    schema.includeGeometryWkt(true);
    CsvWriter csv(out.utf8().c_str());
    csv.schema(schema).write(store(), ObjectQuery());
    const std::vector<Row> rows = parseCsv(readText(out.path()));
    ASSERT_EQ(rows.size(), 5u);
    EXPECT_EQ(rows[0][10], "geometry_wkt");
    EXPECT_EQ(rows[1][10],
              "POLYGON((0 0,100 0,100 100,0 100,0 0),(10 10,20 10,20 20,10 20,10 10))");
    // The two-part object's anchor (200, 0) has y 0, so it sorts before the cells.
    EXPECT_EQ(rows[2][10],
              "MULTIPOLYGON(((200 0,210 0,210 10,200 10,200 0)),"
              "((220 0,240 0,240 20,220 20,220 0),(225 5,230 5,230 10,225 10,225 5)))");
    EXPECT_EQ(rows[3][10], "POLYGON((30 30,40 30,40 40,30 40,30 30))");
}

TEST(CsvWriter, UncalibratedStoreKeepsPixelColumns) {
    StoreFilesGuard files;
    Configuration config;
    StorePtr store = Store::open(files.utf8().c_str(), OpenMode::CreateNew, config);
    const LayerId layer = store->layers().declareSemantic("cells", store->layers().pixelLayer());
    const StageId stage = store->stageStateWriter().declare("seg", "{}");
    ObjectRecord record;
    record.setLayer(layer);
    record.setGeometry(square(0, 0, 4));
    record.measurements().set("area", 16.0, MeasurementUnit::SquarePixels);
    ObjectWriterPtr writer = store->objects().writerForStage(stage);
    writer->emit(record);
    writer->flush();
    store->stageStateWriter().setStatus(stage, StageStatus::Complete);

    for (UnitPolicy policy : {UnitPolicy::Calibrated, UnitPolicy::BothColumns}) {
        OutputPath out;
        ColumnSchema schema;
        schema.units(policy).includeLayer(false).includeStage(false).includeLabel(false)
            .includeBoundingBox(false).allMeasurements();
        CsvWriter(out.utf8().c_str()).schema(schema).provenanceHeader(false).write(*store,
                                                                                   ObjectQuery());
        const std::vector<Row> rows = parseCsv(readText(out.path()));
        ASSERT_EQ(rows.size(), 2u);
        EXPECT_EQ(rows[0], (Row{"layer_id", "anchor_id", "area"}));
        EXPECT_EQ(rows[1][2], "16");
    }
    store->close();
}

TEST_F(CsvExportTest, BothColumnsWritesPixelAndTwin) {
    ColumnSchema schema;
    schema.units(UnitPolicy::BothColumns).allMeasurements();
    const std::vector<Row> rows = parseCsv(exportText(schema));
    ASSERT_EQ(rows.size(), 4u);
    EXPECT_EQ(rows[0], (Row{"layer_id", "anchor_id", "layer", "stage", "labels",
                            "labels_local", "bbox_x", "bbox_y", "bbox_width", "bbox_height",
                            "area", "area.um2", "perimeter"}));
    EXPECT_EQ(rows[1][10], "9900");
    EXPECT_EQ(rows[1][11], "2475");
    EXPECT_EQ(rows[1][12], "");
}

TEST_F(CsvExportTest, ProvenanceStaysOneLinePerStage) {
    const std::vector<std::string> all = lines(exportText(ColumnSchema()));
    std::vector<std::string> comments;
    for (const std::string& line : all) {
        if (!line.empty() && line[0] == '#') comments.push_back(line);
    }
    ASSERT_EQ(comments.size(), 7u);
    EXPECT_EQ(comments[5], "# stage: seg {\\n\"k\":1\\r\\n}");
    EXPECT_EQ(comments[6], "# stage: classify {}");
    for (const std::string& line : all) EXPECT_EQ(line.find('\r'), std::string::npos);

    const std::string bare = exportText(ColumnSchema(), ',', false);
    EXPECT_EQ(bare.rfind("layer_id,", 0), 0u);
    EXPECT_EQ(bare.find('#'), std::string::npos);
}

TEST_F(CsvExportTest, ExportIsDeterministic) {
    ColumnSchema schema;
    schema.includeGeometryWkt(true).includeParent(true).allMeasurements();
    const std::string first = exportText(schema);
    const std::string second = exportText(schema);
    EXPECT_EQ(first, second);
    OutputPath other;
    CsvWriter(other.utf8().c_str()).schema(schema).write(store(), ObjectQuery());
    EXPECT_EQ(readText(other.path()), first);
}

TEST_F(CsvExportTest, CancelledExportLeavesNoFile) {
    CancellationSource source;
    source.cancel();
    RecordingObserver observer;
    const ExportResult result =
        CsvWriter(out.utf8().c_str()).write(store(), ObjectQuery(), &observer, source.token());
    EXPECT_TRUE(result.wasCancelled());
    EXPECT_EQ(result.objectsWritten(), 0);
    EXPECT_EQ(result.bytesWritten(), 0);
    EXPECT_FALSE(std::filesystem::exists(out.path()));
    EXPECT_FALSE(std::filesystem::exists(out.partial()));
}

namespace {
// Cancels its source once `after` objects have been reported.
class CancellingObserver : public ProgressObserver {
public:
    CancellingObserver(CancellationSource& source, int64_t after)
        : source_(source), after_(after) {}
    void onProgress(const char*, double, int64_t completed, int64_t) override {
        if (completed >= after_) source_.cancel();
    }

private:
    CancellationSource& source_;
    int64_t after_;
};
} // namespace

TEST_F(CsvExportTest, CancelledExportKeepsAnExistingFile) {
    writeText(out.path(), "previous export\n");
    CancellationSource source;
    source.cancel();
    const ExportResult result =
        CsvWriter(out.utf8().c_str()).write(store(), ObjectQuery(), nullptr, source.token());
    EXPECT_TRUE(result.wasCancelled());
    EXPECT_EQ(readText(out.path()), "previous export\n");
    EXPECT_FALSE(std::filesystem::exists(out.partial()));
}

TEST_F(CsvExportTest, ReplacesAnExistingFile) {
    writeText(out.path(), "previous export\n");
    const std::string text = exportText(ColumnSchema());
    EXPECT_EQ(text.rfind("# slideio-processor export\n", 0), 0u);
}

TEST_F(CsvExportTest, UnwritablePathThrowsAndLeavesNothing) {
    const std::filesystem::path missing = out.path().parent_path() / "no-such-dir-csv" / "x.csv";
    std::filesystem::path partial = missing;
    partial += ".partial";
    CsvWriter writer(missing.u8string().c_str());
    EXPECT_THROW(writer.write(store(), ObjectQuery()), IoError);
    EXPECT_FALSE(std::filesystem::exists(missing));
    EXPECT_FALSE(std::filesystem::exists(partial));
}

TEST(CsvWriter, ReportsProgress) {
    StoreFilesGuard files;
    Configuration config;
    StorePtr store = Store::open(files.utf8().c_str(), OpenMode::CreateNew, config);
    const LayerId layer = store->layers().declareSemantic("cells", store->layers().pixelLayer());
    const StageId stage = store->stageStateWriter().declare("seg", "{}");
    ObjectWriterPtr writer = store->objects().writerForStage(stage);
    for (int64_t i = 0; i < 2500; ++i) {
        ObjectRecord record;
        record.setLayer(layer);
        record.setGeometry(square(3 * (i % 100), 3 * (i / 100), 2));
        writer->emit(record);
    }
    writer->flush();
    store->stageStateWriter().setStatus(stage, StageStatus::Complete);

    OutputPath out;
    RecordingObserver observer;
    const ExportResult result = CsvWriter(out.utf8().c_str())
                                    .write(*store, ObjectQuery(), &observer, CancellationToken());
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
    OutputPath cancelledOut;
    CancellationSource source;
    CancellingObserver canceller(source, 1000);
    const ExportResult cancelled = CsvWriter(cancelledOut.utf8().c_str())
                                       .write(*store, ObjectQuery(), &canceller, source.token());
    EXPECT_TRUE(cancelled.wasCancelled());
    EXPECT_EQ(cancelled.objectsWritten(), 1000);
    EXPECT_FALSE(std::filesystem::exists(cancelledOut.path()));
    EXPECT_FALSE(std::filesystem::exists(cancelledOut.partial()));
    store->close();
}

TEST(CsvWriter, RefusesBadDelimiter) {
    CsvWriter writer("unused.csv");
    EXPECT_THROW(writer.delimiter('"'), InvalidArgumentError);
    EXPECT_THROW(writer.delimiter('\n'), InvalidArgumentError);
    EXPECT_THROW(writer.delimiter('\r'), InvalidArgumentError);
    EXPECT_THROW(writer.delimiter('#'), InvalidArgumentError);
    EXPECT_NO_THROW(writer.delimiter('\t'));
    EXPECT_THROW(CsvWriter(nullptr), InvalidArgumentError);
    EXPECT_THROW(CsvWriter(""), InvalidArgumentError);
}

TEST_F(CsvExportTest, CustomDelimiter) {
    const std::string text = exportText(ColumnSchema(), ';', false);
    const std::vector<std::string> all = lines(text);
    ASSERT_EQ(all.size(), 4u);
    EXPECT_EQ(all[0],
              "layer_id;anchor_id;layer;stage;labels;labels_local;bbox_x;bbox_y;bbox_width;"
              "bbox_height");
    EXPECT_NE(all[3].find(";\"tumour;myLocal\";\"0;1\";"), std::string::npos) << all[3];
    const std::vector<Row> rows = parseCsv(text, ';');
    EXPECT_EQ(rows[3][4], "tumour;myLocal");
}

namespace {
// Commits one record under a new, completed stage.
void commitOne(Store& store, const char* stageName, const ObjectRecord& record) {
    const StageId stage = store.stageStateWriter().declare(stageName, "{}");
    ObjectWriterPtr writer = store.objects().writerForStage(stage);
    writer->emit(record);
    writer->flush();
    store.stageStateWriter().setStatus(stage, StageStatus::Complete);
}

// Exports every object with `schema`, without provenance, and returns the text.
std::string exportAll(Store& store, const OutputPath& out, const ColumnSchema& schema) {
    const ExportResult result =
        CsvWriter(out.utf8().c_str()).schema(schema).provenanceHeader(false).write(store,
                                                                                   ObjectQuery());
    EXPECT_FALSE(result.wasCancelled());
    const std::string text = readText(out.path());
    EXPECT_EQ(result.bytesWritten(), static_cast<int64_t>(text.size()));
    return text;
}
} // namespace

// pandas.read_csv(comment='#') cuts a line at an unquoted '#': such names are quoted.
TEST_F(CsvExportTest, HashInNamesIsQuoted) {
    const LayerId hashLayer = store().layers().declareSemantic("cells #2", regions);
    const LabelId hashLabel = store().vocabulary().addLocal("class #1");
    ObjectRecord record;
    record.setLayer(hashLayer);
    record.setGeometry(square(300, 0, 4));
    record.setLabel(hashLabel);
    record.measurements().set("size #", 16.0, MeasurementUnit::SquarePixels);
    commitOne(store(), "more", record);

    ColumnSchema schema;
    schema.measurement("size #");
    const std::string text = exportAll(store(), out, schema);
    EXPECT_NE(text.find(",\"size #\"\n"), std::string::npos) << text;
    EXPECT_NE(text.find(",\"cells #2\",more,\"class #1\",1,"), std::string::npos) << text;

    const std::vector<Row> rows = parseCsv(text);
    ASSERT_EQ(rows.size(), 5u);
    EXPECT_EQ(rows[0].back(), "size #");
    bool found = false;
    for (const Row& row : rows) {
        if (row.size() > 2 && row[2] == "cells #2") {
            found = true;
            ASSERT_EQ(row.size(), 11u);
            EXPECT_EQ(row[4], "class #1");
            EXPECT_EQ(row[10], "16");
        }
    }
    EXPECT_TRUE(found);
    const std::string line = header(schema);
    EXPECT_EQ(line.substr(line.size() - 9), ",\"size #\"") << line;
}

// Infinities go through the store end to end. A store cannot hold NaN (SQLite binds it
// as NULL), so the "nan" spelling is checked on the formatter directly.
TEST_F(CsvExportTest, NonFiniteMeasurements) {
    ObjectRecord record;
    record.setLayer(cells);
    record.setGeometry(square(300, 0, 4));
    record.measurements().set("m_pinf", std::numeric_limits<double>::infinity(),
                              MeasurementUnit::Pixels);
    record.measurements().set("m_ninf", -std::numeric_limits<double>::infinity(),
                              MeasurementUnit::Pixels);
    commitOne(store(), "more", record);

    ColumnSchema schema;
    schema.includeLayer(false).includeStage(false).includeLabel(false).includeBoundingBox(false)
        .measurement("m_pinf").measurement("m_ninf");
    const std::vector<Row> rows = parseCsv(exportAll(store(), out, schema));
    bool found = false;
    for (const Row& row : rows) {
        if (row.size() == 4 && row[2] == "inf") {
            found = true;
            EXPECT_EQ(row[3], "-inf");
        }
    }
    EXPECT_TRUE(found);
}

TEST(CsvFormat, NonFiniteValues) {
    std::string out;
    detail::appendDouble(out, std::numeric_limits<double>::quiet_NaN());
    EXPECT_EQ(out, "nan");
    out.clear();
    detail::appendDouble(out, -std::numeric_limits<double>::quiet_NaN());
    EXPECT_EQ(out, "nan");
    out.clear();
    detail::appendDouble(out, std::numeric_limits<double>::infinity());
    EXPECT_EQ(out, "inf");
    out.clear();
    detail::appendDouble(out, -std::numeric_limits<double>::infinity());
    EXPECT_EQ(out, "-inf");
    out.clear();
    detail::appendDouble(out, 0.1);
    EXPECT_EQ(out, "0.10000000000000001");
}

TEST_F(CsvExportTest, FixedColumnNamedMeasurementIsSkipped) {
    ObjectRecord record;
    record.setLayer(cells);
    record.setGeometry(square(300, 0, 4));
    record.measurements().set("labels", 1.0, MeasurementUnit::Dimensionless);
    record.measurements().set("layer_id", 2.0, MeasurementUnit::Dimensionless);
    record.measurements().set("parent_id", 3.0, MeasurementUnit::Dimensionless);
    commitOne(store(), "more", record);

    ColumnSchema schema;
    schema.allMeasurements();
    const std::vector<Row> rows = parseCsv(exportAll(store(), out, schema));
    ASSERT_FALSE(rows.empty());
    EXPECT_EQ(rows[0], (Row{"layer_id", "anchor_id", "layer", "stage", "labels",
                            "labels_local", "bbox_x", "bbox_y", "bbox_width", "bbox_height",
                            "area.um2", "perimeter"}));
}
