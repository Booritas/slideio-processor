// Requirements: UR-RES-7
#pragma once

// A store of labelled squares on one layer, and helpers shared by the masks tests.

#include "common/temp_path.hpp"
#include "storage/store_fixture.hpp"

#include <slideio/processor/core/block_view.hpp>
#include <slideio/processor/core/geometry.hpp>
#include <slideio/processor/core/ids.hpp>
#include <slideio/processor/core/object_query.hpp>
#include <slideio/processor/core/object_record.hpp>
#include <slideio/processor/storage/layer_registry.hpp>
#include <slideio/processor/storage/object_store.hpp>
#include <slideio/processor/storage/stage_state_writer.hpp>
#include <slideio/processor/storage/store.hpp>

#include <slideio/slideio/scene.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <tuple>
#include <vector>

namespace slideio::processor::masks_test {

inline Polygon square(int64_t x, int64_t y, int64_t side) {
    const Point64 points[] = {{x, y}, {x + side, y}, {x + side, y + side}, {x, y + side}};
    Polygon polygon;
    polygon.addRing(points, 4, false);
    return polygon;
}

// A labelled square on `layer`.
inline ObjectRecord labelled(LayerId layer, int64_t x, int64_t y, int64_t side, int32_t label) {
    ObjectRecord r;
    r.setLayer(layer);
    r.setGeometry(square(x, y, side));
    r.setLabel(LabelId(label));
    return r;
}

// A directory removed, with everything in it, at scope exit; the output file lives in it.
struct OutputDir {
    std::filesystem::path dir = uniqueTempPath("masks", "");
    std::filesystem::path tif = dir / "mask.ome.tif";
    OutputDir() { std::filesystem::create_directories(dir); }
    ~OutputDir() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
    OutputDir(const OutputDir&) = delete;
    OutputDir& operator=(const OutputDir&) = delete;
    std::string utf8() const { return tif.u8string(); }
    bool empty() const { return std::filesystem::is_empty(dir); }
};

// Reads a whole scene, or a level of it, as raw interleaved samples.
inline std::vector<uint8_t> readAll(slideio::Scene& scene, size_t sampleBytes) {
    const std::tuple<int, int, int, int> rect = scene.getRect();
    const int w = std::get<2>(rect);
    const int h = std::get<3>(rect);
    std::vector<uint8_t> pixels(static_cast<size_t>(w) * static_cast<size_t>(h) * sampleBytes *
                                static_cast<size_t>(scene.getNumChannels()));
    scene.readBlock(std::make_tuple(0, 0, w, h), pixels.data(), pixels.size());
    return pixels;
}

// A new store holds the ten standard labels, ids 1 to 10, so the squares use those.
// Layer "nuclei" holds four complete labelled squares in [0, 1000) x [0, 800) -- two of
// them overlapping, the lower ObjectId carrying the larger label, and one running past the
// region's far corner -- and one square wholly outside that region.
class MaskStoreTest : public StoreFixture {
protected:
    static constexpr int32_t kWidth = 1000;
    static constexpr int32_t kHeight = 800;

    void SetUp() override {
        StoreFixture::SetUp();
        nuclei = store().layers().declareSemantic("nuclei", store().layers().pixelLayer());
        StageId stage = store().stageStateWriter().declare("segment", "{}");
        ObjectWriterPtr writer = store().objects().writerForStage(stage);
        ObjectRecord a = labelled(nuclei, 100, 100, 200, 9);
        ObjectRecord b = labelled(nuclei, 250, 250, 200, 3);
        ObjectRecord c = labelled(nuclei, 600, 450, 150, 5);
        ObjectRecord d = labelled(nuclei, 930, 730, 140, 7);
        ObjectRecord outside = labelled(nuclei, 1100, 100, 60, 10);
        for (ObjectRecord* r : {&a, &b, &c, &d, &outside}) writer->emit(*r);
        writer->flush();
        first = a.id();
        second = b.id();
        store().stageStateWriter().setStatus(stage, StageStatus::Complete);
    }

    // Adds one more complete labelled square, in a stage of its own.
    void addLate(int64_t x, int64_t y, int64_t side, int32_t label) {
        StageId stage = store().stageStateWriter().declare("late", "{}");
        ObjectWriterPtr writer = store().objects().writerForStage(stage);
        ObjectRecord r = labelled(nuclei, x, y, side, label);
        writer->emit(r);
        writer->flush();
        store().stageStateWriter().setStatus(stage, StageStatus::Complete);
    }

    ObjectQuery all() const {
        ObjectQuery q;
        q.layer(nuclei);
        return q;
    }

    LayerId nuclei;
    ObjectId first, second;
};

} // namespace slideio::processor::masks_test
