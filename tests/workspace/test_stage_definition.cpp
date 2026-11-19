// Requirements: UR-STG-2, UR-STG-3, UR-STG-4, UR-STG-10, UR-IMG-5, UR-RES-11
#include "workspace/support/test_slide.hpp"

#include <workspace/detail/stage_record.hpp>

#include <slideio/processor/config/configuration.hpp>
#include <slideio/processor/core/data_type.hpp>
#include <slideio/processor/core/error.hpp>
#include <slideio/processor/pipeline/operation_parameters.hpp>
#include <slideio/processor/pipeline/pipeline_spec.hpp>
#include <slideio/processor/storage/layer_registry.hpp>
#include <slideio/processor/storage/raster_store.hpp>
#include <slideio/processor/storage/store.hpp>
#include <slideio/processor/workspace/stage_definition.hpp>

#include <gtest/gtest.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <filesystem>
#include <functional>
#include <string>

using namespace slideio::processor;
using testing_support::TempDirectory;

namespace {

// A store in a temporary directory holding the layers and rasters the tests name.
struct Fixture {
    Fixture() {
        std::filesystem::create_directories(dir.path());
        const std::string path = (dir.path() / "test.store").u8string();
        store = Store::open(path.c_str(), OpenMode::CreateNew, Configuration::defaults());
        LayerRegistry& l = store->layers();
        tissue = l.declareSemantic("tissue", l.pixelLayer());
        nests = l.declareSemantic("nests_coarse", l.pixelLayer());
        nuclei = l.declareSemantic("nuclei", tissue);
        cells = l.declareSemantic("cells", tissue);
        probability = store->rasters().declare(RasterDeclaration("probability", DataType::Float32, 1));
        outMap = store->rasters().declare(RasterDeclaration("out_map", DataType::Byte, 1));
    }
    ~Fixture() { store->close(); }

    TempDirectory dir;
    StorePtr store;
    LayerId tissue, nests, nuclei, cells;
    RasterId probability, outMap;
};

PipelineSpec sampleSpec() {
    OperationParameters p;
    p.setDouble("sigma", 1.5).setString("targetLayer", "nuclei");
    PipelineSpec spec;
    spec.analysisLevel(1).operation("pixel.blur", p);
    return spec;
}

StageDefinition fullDefinition(const Fixture& f) {
    StageDefinition def("nuclei");
    def.readsLayer(f.cells)
        .writesLayer(f.nuclei)
        .annotatesLayer(f.cells)
        .dependsOn("background")
        .pipeline(sampleSpec())
        .writesRaster(f.outMap)
        .readsRaster("prob", f.probability)
        .readsLayerAsRaster("coarse", f.nests, LayerRasterContent::ObjectAnchor)
        .restrictToLayer(f.tissue);
    return def;
}

std::string messageOf(const std::function<void()>& call) {
    try {
        call();
    } catch (const std::exception& e) {
        return e.what();
    }
    return std::string();
}

bool contains(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }

std::string replaced(std::string text, const std::string& from, const std::string& to) {
    const size_t at = text.find(from);
    if (at != std::string::npos) text.replace(at, from.size(), to);
    return text;
}

} // namespace

TEST(StageDefinition, AccessorsReflectDeclarations) {
    Fixture f;
    StageDefinition def = fullDefinition(f);
    EXPECT_STREQ(def.name(), "nuclei");
    EXPECT_TRUE(def.pipeline().equals(sampleSpec()));

    ASSERT_EQ(def.writtenLayerCount(), 1);
    EXPECT_EQ(def.writtenLayerAt(0), f.nuclei);
    ASSERT_EQ(def.annotatedLayerCount(), 1);
    EXPECT_EQ(def.annotatedLayerAt(0), f.cells);

    // cells (explicit and annotated), nests_coarse (raster read), tissue (restriction): each once, in order.
    ASSERT_EQ(def.readLayerCount(), 3);
    EXPECT_EQ(def.readLayerAt(0), f.cells);
    EXPECT_EQ(def.readLayerAt(1), f.nests);
    EXPECT_EQ(def.readLayerAt(2), f.tissue);

    EXPECT_TRUE(def.writesAnyRaster());
    EXPECT_EQ(def.writtenRaster(), f.outMap);
    ASSERT_EQ(def.readRasterCount(), 1);
    EXPECT_STREQ(def.readRasterBindingAt(0), "prob");
    EXPECT_EQ(def.readRasterAt(0), f.probability);

    ASSERT_EQ(def.rasterLayerCount(), 1);
    EXPECT_STREQ(def.rasterBindingAt(0), "coarse");
    EXPECT_EQ(def.rasterLayerAt(0), f.nests);
    EXPECT_EQ(def.rasterContentAt(0), LayerRasterContent::ObjectAnchor);

    ASSERT_EQ(def.explicitDependencyCount(), 1);
    EXPECT_STREQ(def.explicitDependencyAt(0), "background");
    EXPECT_TRUE(def.hasLayerRestriction());
    EXPECT_EQ(def.layerRestriction(), f.tissue);

    EXPECT_THROW(def.readLayerAt(3), InvalidArgumentError);
    EXPECT_THROW(def.writtenLayerAt(-1), InvalidArgumentError);
}

TEST(StageDefinition, ImpliedReadsAppearOnceAndNeverAsWrites) {
    Fixture f;
    StageDefinition annotator("a");
    annotator.annotatesLayer(f.cells);
    EXPECT_EQ(annotator.readLayerCount(), 1);
    EXPECT_EQ(annotator.writtenLayerCount(), 0);

    StageDefinition restricted("b");
    restricted.restrictToLayer(f.tissue);
    EXPECT_EQ(restricted.readLayerCount(), 1);
    EXPECT_EQ(restricted.writtenLayerCount(), 0);

    StageDefinition raster("c");
    raster.readsLayerAsRaster("m", f.nests, LayerRasterContent::Membership);
    EXPECT_EQ(raster.readLayerCount(), 1);
    EXPECT_EQ(raster.writtenLayerCount(), 0);

    StageDefinition empty("d");
    EXPECT_FALSE(empty.hasLayerRestriction());
    EXPECT_FALSE(empty.writesAnyRaster());
    EXPECT_EQ(empty.readLayerCount(), 0);
}

TEST(StageDefinition, SecondWrittenRasterRefused) {
    Fixture f;
    StageDefinition def("s");
    def.writesRaster(f.outMap);
    EXPECT_THROW(def.writesRaster(f.probability), InvalidArgumentError);
    EXPECT_EQ(def.writtenRaster(), f.outMap);
}

TEST(StageDefinition, BindingNameNamesOneInput) {
    Fixture f;
    StageDefinition def("s");
    def.readsRaster("prob", f.probability).readsLayerAsRaster("coarse", f.nests, LayerRasterContent::Label);
    // An exact repeat is a no-op.
    def.readsRaster("prob", f.probability).readsLayerAsRaster("coarse", f.nests, LayerRasterContent::Label);
    EXPECT_EQ(def.readRasterCount(), 1);
    EXPECT_EQ(def.rasterLayerCount(), 1);

    // The same name for another target, in either list, is refused and names both.
    const std::string message = messageOf([&] { def.readsRaster("prob", f.outMap); });
    EXPECT_NE(message.find("'s'"), std::string::npos);
    EXPECT_NE(message.find("'prob'"), std::string::npos);
    EXPECT_THROW(def.readsLayerAsRaster("coarse", f.nests, LayerRasterContent::Membership), InvalidArgumentError);
    EXPECT_THROW(def.readsLayerAsRaster("coarse", f.tissue, LayerRasterContent::Label), InvalidArgumentError);
    EXPECT_THROW(def.readsLayerAsRaster("prob", f.nests, LayerRasterContent::Label), InvalidArgumentError);
    EXPECT_THROW(def.readsRaster("coarse", f.probability), InvalidArgumentError);
    EXPECT_EQ(def.readRasterCount(), 1);
    EXPECT_EQ(def.rasterLayerCount(), 1);
    EXPECT_EQ(def.readLayerCount(), 1);
}

TEST(StageDefinition, NullOrEmptyNamesRefused) {
    EXPECT_THROW(StageDefinition(nullptr), InvalidArgumentError);
    EXPECT_THROW(StageDefinition(""), InvalidArgumentError);
    StageDefinition def("s");
    EXPECT_THROW(def.dependsOn(nullptr), InvalidArgumentError);
    EXPECT_THROW(def.readsRaster("", RasterId(1)), InvalidArgumentError);
    EXPECT_THROW(def.readsLayerAsRaster(nullptr, LayerId(1), LayerRasterContent::Label), InvalidArgumentError);
}

TEST(StageDefinition, CopyIsIndependent) {
    Fixture f;
    StageDefinition original = fullDefinition(f);
    StageDefinition copy(original);
    copy.writesLayer(f.cells).dependsOn("other").restrictToLayer(f.nests);
    EXPECT_EQ(original.writtenLayerCount(), 1);
    EXPECT_EQ(original.explicitDependencyCount(), 1);
    EXPECT_EQ(original.layerRestriction(), f.tissue);
    EXPECT_EQ(copy.writtenLayerCount(), 2);

    StageDefinition assigned("x");
    assigned = original;
    assigned.dependsOn("another");
    EXPECT_EQ(original.explicitDependencyCount(), 1);
    EXPECT_EQ(assigned.explicitDependencyCount(), 2);
    EXPECT_STREQ(assigned.name(), "nuclei");
    EXPECT_TRUE(detail::sameDeclaration(original, StageDefinition(original)));
}

TEST(StageRecord, RoundTripsEveryField) {
    Fixture f;
    const StageDefinition def = fullDefinition(f);
    const std::string json = detail::stageRecordJson(def, f.store->layers(), f.store->rasters());

    rapidjson::Document doc;
    doc.Parse(json.c_str());
    ASSERT_FALSE(doc.HasParseError());
    EXPECT_EQ(doc["stageRecordVersion"].GetInt(), 1);
    EXPECT_STREQ(doc["stage"].GetString(), "nuclei");
    ASSERT_EQ(doc["writesLayers"].Size(), 1u);
    EXPECT_STREQ(doc["writesLayers"][0].GetString(), "nuclei");
    EXPECT_STREQ(doc["restrictToLayer"].GetString(), "tissue");
    EXPECT_STREQ(doc["writesRaster"].GetString(), "out_map");
    EXPECT_STREQ(doc["readsLayersAsRaster"][0]["content"].GetString(), "ObjectAnchor");
    EXPECT_STREQ(doc["readsRasters"][0]["raster"].GetString(), "probability");
    ASSERT_TRUE(doc["pipeline"].IsObject());
    EXPECT_STREQ(doc["dependsOn"][0].GetString(), "background");
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> w(sb);
    doc["pipeline"].Accept(w);
    EXPECT_TRUE(PipelineSpec::fromJson(sb.GetString()).equals(sampleSpec()));

    const StageDefinition back = detail::stageFromRecord(json.c_str(), f.store->layers(), f.store->rasters());
    EXPECT_TRUE(detail::sameDeclaration(def, back));
    EXPECT_EQ(detail::stageRecordJson(back, f.store->layers(), f.store->rasters()), json);
}

TEST(StageRecord, EmptyDefinitionRoundTrips) {
    Fixture f;
    const StageDefinition def("bare");
    const std::string json = detail::stageRecordJson(def, f.store->layers(), f.store->rasters());
    EXPECT_TRUE(contains(json, "\"writesRaster\":null"));
    EXPECT_TRUE(contains(json, "\"restrictToLayer\":null"));
    EXPECT_TRUE(detail::sameDeclaration(
        def, detail::stageFromRecord(json.c_str(), f.store->layers(), f.store->rasters())));
}

TEST(StageRecord, ImpliedReadsAreNotRecorded) {
    Fixture f;
    StageDefinition def("s");
    def.restrictToLayer(f.tissue);
    const std::string json = detail::stageRecordJson(def, f.store->layers(), f.store->rasters());
    EXPECT_TRUE(contains(json, "\"readsLayers\":[]")) << json;
    const StageDefinition back = detail::stageFromRecord(json.c_str(), f.store->layers(), f.store->rasters());
    EXPECT_EQ(back.readLayerCount(), 1);
}

TEST(StageRecord, UnknownIdOnWriteNamesTheStage) {
    Fixture f;
    StageDefinition def("mystage");
    def.writesLayer(LayerId(999));
    const std::string message =
        messageOf([&] { detail::stageRecordJson(def, f.store->layers(), f.store->rasters()); });
    EXPECT_TRUE(contains(message, "mystage")) << message;
    StageDefinition rasterDef("rasterstage");
    rasterDef.writesRaster(RasterId(999));
    EXPECT_THROW(detail::stageRecordJson(rasterDef, f.store->layers(), f.store->rasters()),
                 InvalidArgumentError);
}

TEST(StageRecord, UnknownNameOnLoadIsInvalidArgument) {
    Fixture f;
    const std::string json =
        detail::stageRecordJson(fullDefinition(f), f.store->layers(), f.store->rasters());
    const std::string badLayer =
        replaced(json, "\"writesLayers\":[\"nuclei\"]", "\"writesLayers\":[\"ghost_layer\"]");
    const std::string m1 = messageOf(
        [&] { detail::stageFromRecord(badLayer.c_str(), f.store->layers(), f.store->rasters()); });
    EXPECT_TRUE(contains(m1, "ghost_layer")) << m1;
    EXPECT_THROW(detail::stageFromRecord(badLayer.c_str(), f.store->layers(), f.store->rasters()),
                 InvalidArgumentError);
    const std::string badRaster = replaced(json, "\"raster\":\"probability\"", "\"raster\":\"ghost_raster\"");
    const std::string m2 = messageOf(
        [&] { detail::stageFromRecord(badRaster.c_str(), f.store->layers(), f.store->rasters()); });
    EXPECT_TRUE(contains(m2, "ghost_raster")) << m2;
}

TEST(StageRecord, WrongVersionIsInvalidArgument) {
    Fixture f;
    const std::string json =
        detail::stageRecordJson(fullDefinition(f), f.store->layers(), f.store->rasters());
    const std::string v2 = replaced(json, "\"stageRecordVersion\":1", "\"stageRecordVersion\":2");
    EXPECT_THROW(detail::stageFromRecord(v2.c_str(), f.store->layers(), f.store->rasters()),
                 InvalidArgumentError);
}

TEST(StageRecord, MalformedTextIsInvalidArgument) {
    Fixture f;
    LayerRegistry& l = f.store->layers();
    RasterRegistry& r = f.store->rasters();
    EXPECT_THROW(detail::stageFromRecord(nullptr, l, r), InvalidArgumentError);
    EXPECT_THROW(detail::stageFromRecord("{not json", l, r), InvalidArgumentError);
    EXPECT_THROW(detail::stageFromRecord("[]", l, r), InvalidArgumentError);
    EXPECT_THROW(detail::stageFromRecord("{\"stageRecordVersion\":1}", l, r), InvalidArgumentError);
    const std::string json = detail::stageRecordJson(fullDefinition(f), l, r);
    const std::string badContent = replaced(json, "ObjectAnchor", "Bogus");
    EXPECT_THROW(detail::stageFromRecord(badContent.c_str(), l, r), InvalidArgumentError);
}

TEST(StageRecord, SameDeclarationIgnoresListOrderAndJsonFormatting) {
    Fixture f;
    StageDefinition ab("s");
    ab.readsLayer(f.tissue).readsLayer(f.cells).dependsOn("x").dependsOn("y");
    StageDefinition ba("s");
    ba.readsLayer(f.cells).readsLayer(f.tissue).dependsOn("y").dependsOn("x");
    EXPECT_TRUE(detail::sameDeclaration(ab, ba));

    StageDefinition restrictedA("s");
    restrictedA.restrictToLayer(f.tissue);
    StageDefinition restrictedB("s");
    restrictedB.restrictToLayer(f.cells);
    EXPECT_FALSE(detail::sameDeclaration(restrictedA, restrictedB));
    EXPECT_FALSE(detail::sameDeclaration(restrictedA, StageDefinition("s")));

    // An explicit read that another declaration also implies is still a different declaration.
    StageDefinition explicitAndRestricted("s");
    explicitAndRestricted.readsLayer(f.tissue).restrictToLayer(f.tissue);
    EXPECT_FALSE(detail::sameDeclaration(explicitAndRestricted, restrictedA));

    // A record reformatted with whitespace loads to the same declaration.
    const StageDefinition def = fullDefinition(f);
    const std::string json = detail::stageRecordJson(def, f.store->layers(), f.store->rasters());
    const std::string spaced = replaced(replaced(json, ",\"stage\"", " ,\n \"stage\""),
                                        "{\"stageRecordVersion\"", "{ \"stageRecordVersion\"");
    EXPECT_NE(spaced, json);
    EXPECT_TRUE(detail::sameDeclaration(
        def, detail::stageFromRecord(spaced.c_str(), f.store->layers(), f.store->rasters())));
}

TEST(StageRecord, SameDeclarationComparesEveryField) {
    Fixture f;
    const StageDefinition base = fullDefinition(f);
    auto same = [&](const std::function<void(StageDefinition&)>& change) {
        StageDefinition d = base;
        change(d);
        return detail::sameDeclaration(base, d);
    };
    EXPECT_TRUE(same([](StageDefinition&) {}));
    EXPECT_FALSE(same([&](StageDefinition& d) { d.writesLayer(f.cells); }));
    EXPECT_FALSE(same([&](StageDefinition& d) { d.annotatesLayer(f.tissue); }));
    EXPECT_FALSE(same([&](StageDefinition& d) { d.dependsOn("z"); }));
    EXPECT_FALSE(same([&](StageDefinition& d) { d.readsRaster("other", f.probability); }));
    EXPECT_FALSE(same([&](StageDefinition& d) {
        d.readsLayerAsRaster("fine", f.nests, LayerRasterContent::Label);
    }));
    EXPECT_FALSE(same([&](StageDefinition& d) { d.pipeline(PipelineSpec()); }));
    EXPECT_FALSE(detail::sameDeclaration(StageDefinition("a"), StageDefinition("b")));
}

TEST(StageDefinition, ReplacedRestrictionIsNoLongerRead) {
    Fixture f;
    StageDefinition def("s");
    def.restrictToLayer(f.tissue).restrictToLayer(f.cells);
    ASSERT_EQ(def.readLayerCount(), 1);
    EXPECT_EQ(def.readLayerAt(0), f.cells);

    // A layer that is also read another way stays read, in its first-declaration place.
    StageDefinition kept("k");
    kept.readsLayer(f.tissue).restrictToLayer(f.tissue).restrictToLayer(f.cells);
    ASSERT_EQ(kept.readLayerCount(), 2);
    EXPECT_EQ(kept.readLayerAt(0), f.tissue);
    EXPECT_EQ(kept.readLayerAt(1), f.cells);

    // Reads agree with those of the definition rebuilt from its record.
    const std::string json = detail::stageRecordJson(def, f.store->layers(), f.store->rasters());
    const StageDefinition back = detail::stageFromRecord(json.c_str(), f.store->layers(), f.store->rasters());
    ASSERT_EQ(back.readLayerCount(), 1);
    EXPECT_EQ(back.readLayerAt(0), f.cells);
}

TEST(StageRecord, MalformedPipelineIsInvalidArgument) {
    Fixture f;
    LayerRegistry& l = f.store->layers();
    RasterRegistry& r = f.store->rasters();
    const std::string json = detail::stageRecordJson(StageDefinition("s"), l, r);
    const size_t at = json.find("\"pipeline\":");
    ASSERT_NE(at, std::string::npos);
    const std::string head = json.substr(0, at);
    EXPECT_THROW(detail::stageFromRecord((head + "\"pipeline\":[]}").c_str(), l, r), InvalidArgumentError);
    EXPECT_THROW(detail::stageFromRecord((head + "\"pipeline\":{\"specVersion\":7}}").c_str(), l, r),
                 InvalidArgumentError);
    EXPECT_THROW(detail::stageFromRecord(head.substr(0, head.size() - 1).append("}").c_str(), l, r),
                 InvalidArgumentError);
}
