// Requirements: UR-RES-9, UR-RES-15, UR-SEG-6
#include "storage/store_fixture.hpp"

#include "reporting/detail/columns.hpp"

#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/geometry.hpp>
#include <slideio/processor/reporting/reporting.hpp>
#include <slideio/processor/storage/object_store.hpp>
#include <slideio/processor/storage/store.hpp>

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

using namespace slideio::processor;

namespace {

std::string header(const ColumnSchema& schema) {
    const int32_t n = schema.toHeaderLine(nullptr, 0);
    std::vector<char> buffer(static_cast<size_t>(n) + 1);
    EXPECT_EQ(schema.toHeaderLine(buffer.data(), static_cast<int32_t>(buffer.size())), n);
    return std::string(buffer.data());
}

std::vector<std::string> names(const detail::ResolvedColumns& resolved) {
    std::vector<std::string> out;
    for (const detail::ResolvedColumn& c : resolved.columns) out.push_back(c.name);
    return out;
}

class ColumnResolutionTest : public StoreFixture {
protected:
    void SetUp() override {
        StoreFixture::SetUp();
        const LayerId layer =
            store().layers().declareSemantic("nuclei", store().layers().pixelLayer());
        const StageId stage = store().stageStateWriter().declare("seg", "");
        const Point64 points[] = {{0, 0}, {5, 0}, {5, 5}, {0, 5}};
        Polygon polygon;
        polygon.addRing(points, 4, false);
        ObjectRecord record;
        record.setLayer(layer);
        record.setGeometry(polygon);
        record.measurements().set("perimeter", 20.0, MeasurementUnit::Pixels);
        record.measurements().set("area", 25.0, MeasurementUnit::SquarePixels);
        record.measurements().set("area.um2", 6.25, MeasurementUnit::SquareMicrometres);
        ObjectWriterPtr writer = store().objects().writerForStage(stage);
        writer->emit(record);
        writer->flush();
        store().stageStateWriter().setStatus(stage, StageStatus::Complete);
    }
};

} // namespace

TEST(ColumnSchema, StandardHeader) {
    EXPECT_EQ(header(ColumnSchema::standard()),
              "layer_id,anchor_id,layer,stage,labels,labels_local,bbox_x,bbox_y,bbox_width,"
              "bbox_height");
    EXPECT_EQ(ColumnSchema::standard().columnCount(), 10);
    EXPECT_STREQ(ColumnSchema::standard().columnNameAt(0), "layer_id");
}

TEST(ColumnSchema, PackedIdAndOptionalColumns) {
    ColumnSchema schema;
    schema.idFormat(IdFormat::PackedString).includeGeometryWkt(true).includeParent(true);
    EXPECT_EQ(header(schema),
              "object_id,layer,stage,labels,labels_local,bbox_x,bbox_y,bbox_width,bbox_height,"
              "geometry_wkt,parent_id");
    ColumnSchema two;
    two.includeGeometryWkt(true).includeParent(true);
    EXPECT_EQ(header(two),
              "layer_id,anchor_id,layer,stage,labels,labels_local,bbox_x,bbox_y,bbox_width,"
              "bbox_height,geometry_wkt,parent_layer_id,parent_anchor_id");
}

TEST(ColumnSchema, ProducerOnlyLabelColumns) {
    ColumnSchema schema;
    schema.labelPolicy(LabelPolicy::ProducerOnly);
    EXPECT_EQ(header(schema),
              "layer_id,anchor_id,layer,stage,label,label_local,bbox_x,bbox_y,bbox_width,"
              "bbox_height");
}

TEST(ColumnSchema, DroppedColumnsLeaveOnlyTheIdentifier) {
    ColumnSchema schema;
    schema.includeLayer(false).includeStage(false).includeLabel(false).includeBoundingBox(false);
    EXPECT_EQ(header(schema), "layer_id,anchor_id");
}

TEST(ColumnSchema, DuplicateMeasurementThrows) {
    ColumnSchema schema;
    schema.measurement("area");
    EXPECT_THROW(schema.measurement("area"), InvalidArgumentError);
    EXPECT_EQ(schema.columnCount(), 11);
}

TEST(ColumnSchema, BadMeasurementNamesThrow) {
    ColumnSchema schema;
    EXPECT_THROW(schema.measurement(nullptr), InvalidArgumentError);
    EXPECT_THROW(schema.measurement(""), InvalidArgumentError);
    EXPECT_THROW(schema.measurement("area.um2"), InvalidArgumentError);
    EXPECT_THROW(schema.measurement("length.um"), InvalidArgumentError);
    EXPECT_EQ(schema.columnCount(), 10);
}

TEST(ColumnSchema, FixedColumnNameMeasurementThrows) {
    ColumnSchema schema;
    for (const char* name : {"layer_id", "anchor_id", "object_id", "layer", "stage", "labels",
                             "labels_local", "label", "label_local", "bbox_x", "bbox_y",
                             "bbox_width", "bbox_height", "geometry_wkt", "parent_layer_id",
                             "parent_anchor_id", "parent_id"}) {
        EXPECT_THROW(schema.measurement(name), InvalidArgumentError) << name;
    }
    EXPECT_EQ(schema.columnCount(), 10);
    EXPECT_NO_THROW(schema.measurement("labels_count"));
    EXPECT_EQ(schema.columnCount(), 11);
}

TEST(ColumnSchema, HeaderBufferTooSmall) {
    const ColumnSchema schema = ColumnSchema::standard();
    const std::string full = header(schema);
    char buffer[8];
    std::memset(buffer, 'x', sizeof(buffer));
    EXPECT_EQ(schema.toHeaderLine(buffer, 8), static_cast<int32_t>(full.size()));
    EXPECT_STREQ(buffer, full.substr(0, 7).c_str());
    char untouched[4] = {'a', 'b', 'c', 'd'};
    EXPECT_EQ(schema.toHeaderLine(untouched, 0), static_cast<int32_t>(full.size()));
    EXPECT_EQ(untouched[0], 'a');
}

TEST(ColumnSchema, ColumnNameAtOutOfRangeThrows) {
    const ColumnSchema schema = ColumnSchema::standard();
    EXPECT_THROW(schema.columnNameAt(-1), InvalidArgumentError);
    EXPECT_THROW(schema.columnNameAt(schema.columnCount()), InvalidArgumentError);
}

TEST(ColumnSchema, CopiesAreIndependent) {
    ColumnSchema a;
    a.measurement("area");
    ColumnSchema b = a;
    b.measurement("perimeter");
    EXPECT_EQ(a.columnCount(), 11);
    EXPECT_EQ(b.columnCount(), 12);
    ColumnSchema c = std::move(b);
    EXPECT_EQ(c.columnCount(), 12);
    EXPECT_EQ(b.columnCount(), 10);  // NOLINT: a moved-from schema holds the default
}

TEST(ColumnSchema, AwkwardMeasurementNameIsQuotedInTheHeader) {
    ColumnSchema schema;
    schema.measurement("a,\"b");
    const std::string line = header(schema);
    EXPECT_EQ(line.substr(line.find("bbox_height") + 11), ",\"a,\"\"b\"");
}

TEST_F(ColumnResolutionTest, UnitPoliciesResolveTwins) {
    ColumnSchema calibrated;
    calibrated.allMeasurements();
    const std::vector<std::string> fixed = {"layer_id", "anchor_id", "layer", "stage", "labels",
                                            "labels_local", "bbox_x", "bbox_y", "bbox_width",
                                            "bbox_height"};
    auto withTail = [&](std::vector<std::string> tail) {
        std::vector<std::string> all = fixed;
        all.insert(all.end(), tail.begin(), tail.end());
        return all;
    };
    EXPECT_EQ(names(detail::resolveColumns(calibrated, store())),
              withTail({"area.um2", "perimeter"}));

    ColumnSchema pixels;
    pixels.units(UnitPolicy::PixelsOnly).allMeasurements();
    EXPECT_EQ(names(detail::resolveColumns(pixels, store())), withTail({"area", "perimeter"}));

    ColumnSchema both;
    both.units(UnitPolicy::BothColumns).allMeasurements();
    EXPECT_EQ(names(detail::resolveColumns(both, store())),
              withTail({"area", "area.um2", "perimeter"}));

    const detail::ResolvedColumns resolved = detail::resolveColumns(calibrated, store());
    EXPECT_EQ(resolved.columns[10].kind, detail::ColumnKind::Measurement);
    EXPECT_EQ(resolved.columns[10].measurement, "area.um2");
    EXPECT_EQ(resolved.columns[0].kind, detail::ColumnKind::LayerId);
}

TEST_F(ColumnResolutionTest, ExplicitNamesComeFirst) {
    ColumnSchema schema;
    schema.measurement("perimeter").allMeasurements().allMeasurements();
    const detail::ResolvedColumns resolved = detail::resolveColumns(schema, store());
    ASSERT_EQ(resolved.columns.size(), 12u);
    EXPECT_EQ(resolved.columns[10].name, "perimeter");
    EXPECT_EQ(resolved.columns[11].name, "area.um2");
}

TEST_F(ColumnResolutionTest, AnExplicitNameTheStoreLacksStaysAColumn) {
    ColumnSchema schema;
    schema.measurement("circularity");
    const detail::ResolvedColumns resolved = detail::resolveColumns(schema, store());
    ASSERT_EQ(resolved.columns.size(), 11u);
    EXPECT_EQ(resolved.columns[10].name, "circularity");
    EXPECT_EQ(resolved.columns[10].measurement, "circularity");
}
