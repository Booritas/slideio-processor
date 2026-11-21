// Requirements: UR-RES-8, UR-RES-9, UR-RES-15, UR-QA-3
#pragma once

// The store and helpers shared by the CSV and GeoJSON export tests.

#include "common/temp_path.hpp"
#include "storage/store_fixture.hpp"

#include <slideio/processor/core/control.hpp>
#include <slideio/processor/core/geometry.hpp>
#include <slideio/processor/core/ids.hpp>
#include <slideio/processor/core/measurement.hpp>
#include <slideio/processor/core/object_record.hpp>
#include <slideio/processor/core/provenance.hpp>
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
#include <initializer_list>
#include <iterator>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace slideio::processor::export_test {

// An output path; the file and its ".partial" sibling are removed on destruction.
class OutputPath {
public:
    explicit OutputPath(const char* stem = "export", const char* extension = ".csv")
        : path_(uniqueTempPath(stem, extension)) {}
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

inline std::string readText(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

inline void writeText(const std::filesystem::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary);
    out << text;
}

inline Polygon square(int64_t x, int64_t y, int64_t side) {
    const Point64 points[] = {{x, y}, {x + side, y}, {x + side, y + side}, {x, y + side}};
    Polygon polygon;
    polygon.addRing(points, 4, false);
    return polygon;
}

inline std::string decimal(int64_t value) { return std::to_string(value); }

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

// Declares a stage, writes one object under it and completes it.
inline void commitOne(Store& store, const char* stageName, const ObjectRecord& record) {
    const StageId stage = store.stageStateWriter().declare(stageName, "{}");
    ObjectWriterPtr writer = store.objects().writerForStage(stage);
    writer->emit(record);
    writer->flush();
    store.stageStateWriter().setStatus(stage, StageStatus::Complete);
}

// Declares `stageName`, asserts each (object, value) of "positive_fraction" under it and,
// when `complete`, completes the stage.
inline StageId assertFraction(Store& store, const char* stageName,
                              std::initializer_list<std::pair<ObjectId, double>> values,
                              bool complete = true) {
    const StageId stage = store.stageStateWriter().declare(stageName, "{}");
    AnnotationWriterPtr writer = store.annotations().writerForStage(stage);
    for (const auto& [object, value] : values)
        writer->assertMeasurement(object, "positive_fraction", value,
                                  MeasurementUnit::Dimensionless);
    writer->flush();
    if (complete) store.stageStateWriter().setStatus(stage, StageStatus::Complete);
    return stage;
}

// A region (tumour, with a hole, the root) on the pixel layer and two cells above it.
// Cell 1 has no label and a parent; cell 2 has the row label tumour, an asserted local
// label myLocal, and a parent. Area has calibrated twins; perimeter does not.
class ExportStoreTest : public StoreFixture {
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

    LayerId regions, cells;
    StageId seg, classify;
    LabelId tumour, myLocal;
    ObjectId regionId, cell1Id, cell2Id;
};

} // namespace slideio::processor::export_test
