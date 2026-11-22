// Requirements: UR-API-2, UR-STG-4, UR-RES-11
#include "workspace/support/test_slide.hpp"

#include <slideio/processor/config/configuration.hpp>
#include <slideio/processor/core/data_type.hpp>
#include <slideio/processor/core/error.hpp>
#include <slideio/processor/run/run.hpp>
#include <slideio/processor/storage/layer_registry.hpp>
#include <slideio/processor/storage/raster_store.hpp>
#include <slideio/processor/storage/store.hpp>
#include <slideio/processor/workspace/stage_definition.hpp>
#include <slideio/processor/workspace/stage_registry.hpp>
#include <slideio/processor/workspace/workspace.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <utility>
#include <vector>

using namespace slideio::processor;
using testing_support::TempDirectory;
using testing_support::writeTestSlide;

namespace {

// A stage record (stageRecordVersion 1) that reads and writes the layers and raster named.
std::string record(const std::string& name, const std::string& reads, const std::string& writes,
                   const std::string& raster = "null") {
    return "{\"stageRecordVersion\":1,\"stage\":\"" + name + "\",\"readsLayers\":[" + reads +
           "],\"writesLayers\":[" + writes + "],\"annotatesLayers\":[],\"restrictToLayer\":null,"
           "\"readsLayersAsRaster\":[],\"writesRaster\":" + raster + ",\"readsRasters\":[],\"dependsOn\":[],"
           "\"pipeline\":{\"specVersion\":1,\"channels\":\"all\",\"analysisLevel\":-1,\"operations\":[]}}";
}

// A file body from the pieces given; the stages and exports are JSON array contents.
std::string file(const std::string& layers, const std::string& rasters, const std::string& stages,
                 const std::string& exports, const std::string& version = "1") {
    return "{\"formatVersion\":" + version + ",\"requires\":[\"mylab.models\"],\"layers\":[" + layers +
           "],\"rasters\":[" + rasters + "],\"stages\":[" + stages + "],\"exports\":[" + exports + "]}";
}

const char* kLayers =
    "{\"name\":\"tissue\"},{\"name\":\"nuclei\",\"parent\":\"tissue\",\"kind\":\"semantic\"}";
const char* kRasters = "{\"name\":\"prob\",\"type\":\"Float32\",\"planes\":1,\"compression\":\"Deflate\"}";

// The example of the design: two layers, one raster, two stages and the four export kinds.
std::string example() {
    return file(kLayers, kRasters,
                record("tissue", "", "\"tissue\"") + "," + record("nuclei", "\"tissue\"", "\"nuclei\"", "null"),
                "{\"kind\":\"csv\",\"file\":\"nuclei.csv\",\"layer\":\"nuclei\",\"measurements\":[\"area\"]},"
                "{\"kind\":\"geojson\",\"file\":\"nuclei.geojson\",\"layer\":\"nuclei\"},"
                "{\"kind\":\"labelMask\",\"file\":\"nuclei-mask.tif\",\"layer\":\"nuclei\"},"
                "{\"kind\":\"rasterTiff\",\"file\":\"prob.tif\",\"raster\":\"prob\"}");
}

std::string jsonOf(const PipelineFile& f) {
    const int32_t length = f.toJson(nullptr, 0);
    std::string text(static_cast<size_t>(length) + 1, '\0');
    EXPECT_EQ(f.toJson(&text[0], length + 1), length);
    text.resize(static_cast<size_t>(length));
    return text;
}

// Runs a call that must throw InvalidArgumentError and returns its message.
std::string refusal(const std::function<void()>& call) {
    try {
        call();
    } catch (const InvalidArgumentError& e) {
        return e.what();
    }
    ADD_FAILURE() << "expected InvalidArgumentError";
    return std::string();
}

} // namespace

TEST(PipelineFile, RoundTrips) {
    const PipelineFile first = PipelineFile::fromJson(example().c_str());
    ASSERT_EQ(first.requireCount(), 1);
    EXPECT_STREQ(first.requireAt(0), "mylab.models");
    ASSERT_EQ(first.layerCount(), 2);
    EXPECT_STREQ(first.layerName(1), "nuclei");
    EXPECT_STREQ(first.layerParent(1), "tissue");
    ASSERT_EQ(first.rasterCount(), 1);
    EXPECT_STREQ(first.rasterAt(0).name(), "prob");
    EXPECT_EQ(first.rasterAt(0).planeType(), DataType::Float32);
    ASSERT_EQ(first.stageCount(), 2);
    EXPECT_STREQ(first.stageName(1), "nuclei");
    ASSERT_EQ(first.exportCount(), 4);
    EXPECT_EQ(first.exportAt(0).kind(), ExportKind::Csv);
    ASSERT_EQ(first.exportAt(0).measurementCount(), 1);
    EXPECT_STREQ(first.exportAt(0).measurementAt(0), "area");
    EXPECT_EQ(first.exportAt(1).kind(), ExportKind::GeoJson);
    EXPECT_EQ(first.exportAt(2).kind(), ExportKind::LabelMask);
    EXPECT_EQ(first.exportAt(3).kind(), ExportKind::RasterTiff);
    EXPECT_STREQ(first.exportAt(3).raster(), "prob");
    EXPECT_EQ(first.exportAt(3).layer(), nullptr);

    const std::string text = jsonOf(first);
    const PipelineFile second = PipelineFile::fromJson(text.c_str());
    EXPECT_TRUE(first.equals(second));
    EXPECT_EQ(jsonOf(second), text);
    const PipelineFile third = PipelineFile::fromJson(jsonOf(second).c_str());
    EXPECT_TRUE(second.equals(third));
    EXPECT_EQ(jsonOf(third), text);
}

TEST(PipelineFile, KeepsUnknownStageRecordKeys) {
    std::string stage = record("tissue", "", "\"tissue\"");
    stage.pop_back();
    stage += ",\"nondeterministicOperations\":[\"ml.model\"]}";
    const PipelineFile f = PipelineFile::fromJson(file("{\"name\":\"tissue\"}", "", stage, "").c_str());
    EXPECT_NE(std::string(f.stageRecord(0)).find("\"nondeterministicOperations\":[\"ml.model\"]"), std::string::npos);
    EXPECT_NE(jsonOf(f).find("\"nondeterministicOperations\":[\"ml.model\"]"), std::string::npos);
}

TEST(PipelineFile, DefaultsApply) {
    const std::string body = file("{\"name\":\"tissue\"}", "{\"name\":\"prob\",\"type\":\"Byte\",\"planes\":3}", "",
                                  "{\"kind\":\"csv\",\"file\":\"t.csv\",\"layer\":\"tissue\"}");
    const PipelineFile f = PipelineFile::fromJson(body.c_str());
    EXPECT_EQ(f.layerParent(0), nullptr);
    EXPECT_EQ(f.layerKind(0), LayerKind::Semantic);
    EXPECT_EQ(f.rasterAt(0).compressionValue(), RasterCompression::Deflate);
    EXPECT_EQ(f.rasterAt(0).planeCount(), 3);
    EXPECT_EQ(f.exportAt(0).measurementCount(), 0);

    const PipelineFile primitive = PipelineFile::fromJson(
        file("{\"name\":\"p\",\"kind\":\"primitive\"}", "{\"name\":\"r\",\"type\":\"Byte\",\"planes\":1,\"compression\":\"Lzw\"}",
             "", "").c_str());
    EXPECT_EQ(primitive.layerKind(0), LayerKind::Primitive);
    EXPECT_EQ(primitive.rasterAt(0).compressionValue(), RasterCompression::Lzw);
}

// The JSON body of a file with one csv export of layer "tissue" to `name`, escaped for JSON.
static std::string csvExportFile(const std::string& name) {
    static const char* const hex = "0123456789abcdef";
    std::string escaped;
    for (const char c : name) {
        const auto byte = static_cast<unsigned char>(c);
        if (c == '\\' || c == '"') {
            escaped += '\\';
            escaped += c;
        } else if (byte < 0x20) {
            escaped += "\\u00";
            escaped += hex[byte >> 4];
            escaped += hex[byte & 0xF];
        } else {
            escaped += c;
        }
    }
    return file("{\"name\":\"tissue\"}", "", "", "{\"kind\":\"csv\",\"file\":\"" + escaped + "\",\"layer\":\"tissue\"}");
}

TEST(PipelineFile, RefusesUnsafeExportNames) {
    const std::vector<std::string> unsafe = {
        "", ".", "..", "../x.csv", "C:x.csv", "a/b.csv", "a\\b.csv",
        // The writers' own temporary suffix, and names Windows silently alters.
        "x.partial", "x.PARTIAL", "x.csv.Partial", "x.csv.", "x.csv ", "x.",
        // Control characters.
        "a\tb.csv", "a\nb.csv", "a\x01" "b.csv", "a\x1f" "b.csv", "a\x7f" "b.csv",
        // Windows device names, with or without an extension, in any case.
        "CON", "con", "Prn.csv", "AUX.txt", "nul", "NUL.tar.gz", "COM1", "com9.csv", "LPT1", "lpt9.geojson",
        "CON .csv"};
    for (const std::string& name : unsafe) {
        EXPECT_THROW(ExportSpec::csv(name.c_str(), "tissue"), InvalidArgumentError) << name;
        EXPECT_THROW(PipelineFile::fromJson(csvExportFile(name).c_str()), InvalidArgumentError) << name;
    }
    // An embedded NUL cannot reach ExportSpec through a C string, but can through JSON;
    // read as a C string, "a\u0000/b.csv" would end before its separator.
    EXPECT_THROW(PipelineFile::fromJson(csvExportFile(std::string("a\0/b.csv", 8)).c_str()), InvalidArgumentError);
    EXPECT_THROW(PipelineFile::fromJson(file("{\"name\":\"tis\\u0000sue\"}", "", "", "").c_str()),
                 InvalidArgumentError);
    std::string nulRequire = file("", "", "", "");
    nulRequire.replace(nulRequire.find("mylab.models"), 12, "mylab\\u0000x");
    EXPECT_THROW(PipelineFile::fromJson(nulRequire.c_str()), InvalidArgumentError);
    EXPECT_THROW(PipelineFile::fromJson(
                     file("{\"name\":\"tissue\"}", "", "",
                          "{\"kind\":\"csv\",\"file\":\"t.csv\",\"layer\":\"tissue\",\"measurements\":[\"a\\u0000b\"]}")
                         .c_str()),
                 InvalidArgumentError);

    // Names that merely resemble the refused ones stay valid.
    for (const char* name : {"console.csv", "com10.csv", "com0.csv", "lpt.csv", "nuls.csv", "x.partial.csv",
                             "partial", ".hidden.csv", "a b.csv", "CONTENT"}) {
        EXPECT_NO_THROW(ExportSpec::csv(name, "tissue")) << name;
        EXPECT_NO_THROW(PipelineFile::fromJson(csvExportFile(name).c_str())) << name;
    }
    EXPECT_THROW(ExportSpec::geoJson(nullptr, "tissue"), InvalidArgumentError);
    EXPECT_THROW(ExportSpec::rasterTiff("a:b", "prob"), InvalidArgumentError);
}

TEST(PipelineFile, RefusesDuplicates) {
    const std::string exportOf = "{\"kind\":\"csv\",\"file\":\"A.csv\",\"layer\":\"tissue\"}";
    EXPECT_THROW(PipelineFile::fromJson(file("{\"name\":\"tissue\"},{\"name\":\"tissue\"}", "", "", "").c_str()),
                 InvalidArgumentError);
    const std::string raster = "{\"name\":\"prob\",\"type\":\"Float32\",\"planes\":1}";
    EXPECT_THROW(PipelineFile::fromJson(file("", raster + "," + raster, "", "").c_str()), InvalidArgumentError);
    const std::string stage = record("tissue", "", "\"tissue\"");
    EXPECT_THROW(PipelineFile::fromJson(file("{\"name\":\"tissue\"}", "", stage + "," + stage, "").c_str()),
                 InvalidArgumentError);
    EXPECT_THROW(PipelineFile::fromJson(
                     file("{\"name\":\"tissue\"}", "", "",
                          exportOf + ",{\"kind\":\"geojson\",\"file\":\"a.CSV\",\"layer\":\"tissue\"}")
                         .c_str()),
                 InvalidArgumentError);

    PipelineFile f = PipelineFile::fromJson(file("{\"name\":\"tissue\"}", "", "", exportOf).c_str());
    EXPECT_THROW(f.addExport(ExportSpec::geoJson("a.csv", "tissue")), InvalidArgumentError);
    EXPECT_EQ(f.exportCount(), 1);
}

TEST(PipelineFile, RefusesUnknownReferences) {
    const std::string tissue = "{\"name\":\"tissue\"}";
    // An export of a layer or raster nothing declares or writes.
    std::string message = refusal([&] {
        PipelineFile::fromJson(
            file(tissue, "", "", "{\"kind\":\"csv\",\"file\":\"x.csv\",\"layer\":\"ghostlayer\"}").c_str());
    });
    EXPECT_NE(message.find("layer"), std::string::npos);
    EXPECT_EQ(message.find("ghostlayer"), std::string::npos);
    message = refusal([&] {
        PipelineFile::fromJson(
            file(tissue, "", "", "{\"kind\":\"rasterTiff\",\"file\":\"x.tif\",\"raster\":\"ghostraster\"}").c_str());
    });
    EXPECT_NE(message.find("raster"), std::string::npos);
    EXPECT_EQ(message.find("ghostraster"), std::string::npos);

    // A stage writes only declared layers and rasters (RefusesUndeclaredStageReferences), so
    // an export of what a stage writes names a declared one and is accepted.
    EXPECT_NO_THROW(PipelineFile::fromJson(
        file("{\"name\":\"made\"}", "{\"name\":\"mapr\",\"type\":\"Byte\",\"planes\":1}",
             record("seg", "", "\"made\"", "\"mapr\""),
             "{\"kind\":\"csv\",\"file\":\"x.csv\",\"layer\":\"made\"},"
             "{\"kind\":\"rasterTiff\",\"file\":\"x.tif\",\"raster\":\"mapr\"}")
            .c_str()));

    message = refusal([&] {
        PipelineFile::fromJson(file(tissue, "", "", "{\"kind\":\"pdf\",\"file\":\"x.pdf\",\"layer\":\"tissue\"}").c_str());
    });
    EXPECT_NE(message.find("kind"), std::string::npos);
    EXPECT_EQ(message.find("pdf"), std::string::npos);

    EXPECT_THROW(PipelineFile::fromJson(file(tissue, "", "", "", "2").c_str()), InvalidArgumentError);
    EXPECT_THROW(PipelineFile::fromJson("{\"requires\":[],\"layers\":[],\"rasters\":[],\"stages\":[],\"exports\":[]}"),
                 InvalidArgumentError);
    EXPECT_THROW(PipelineFile::fromJson(file("", "", "", "").replace(0, 1, "[").c_str()), InvalidArgumentError);
    EXPECT_THROW(PipelineFile::fromJson(nullptr), InvalidArgumentError);
    EXPECT_THROW(PipelineFile::fromJson("{"), InvalidArgumentError);

    // A stage whose pipeline does not parse, named in the message.
    std::string bad = record("brokenstage", "", "");
    const size_t at = bad.find("\"specVersion\":1");
    bad.replace(at, std::string("\"specVersion\":1").size(), "\"specVersion\":7");
    message = refusal([&] { PipelineFile::fromJson(file("", "", bad, "").c_str()); });
    EXPECT_NE(message.find("brokenstage"), std::string::npos);

    // A stage entry that is not an object, or lacks its name.
    EXPECT_THROW(PipelineFile::fromJson(file("", "", "3", "").c_str()), InvalidArgumentError);
    EXPECT_THROW(PipelineFile::fromJson(file("", "", "{\"pipeline\":{}}", "").c_str()), InvalidArgumentError);

    // A layer whose parent is not declared, and a mistyped member.
    EXPECT_THROW(PipelineFile::fromJson(file("{\"name\":\"a\",\"parent\":\"b\"}", "", "", "").c_str()),
                 InvalidArgumentError);
    EXPECT_THROW(PipelineFile::fromJson(file("{\"name\":5}", "", "", "").c_str()), InvalidArgumentError);
    EXPECT_THROW(PipelineFile::fromJson(
                     file("", "{\"name\":\"r\",\"type\":\"Quad\",\"planes\":1}", "", "").c_str()),
                 InvalidArgumentError);
}

TEST(PipelineFile, FromWorkspace) {
    TempDirectory dir;
    std::filesystem::create_directories(dir.path());
    const std::filesystem::path slide = writeTestSlide(dir.path() / "slide.tif", 300, 200);
    const std::string wsPath = (dir.path() / "ws").u8string();
    WorkspacePtr ws = Workspace::create(wsPath.c_str(), slide.u8string().c_str(), 0, Configuration::defaults());
    LayerLifecycle& lifecycle = ws->layerLifecycle();
    const LayerId pixel = ws->layers().pixelLayer();
    const LayerId tissue = lifecycle.declareSemantic("tissue", pixel);
    const LayerId nuclei = lifecycle.declareSemantic("nuclei", tissue);
    const RasterId prob = ws->store()->rasters().declare(RasterDeclaration("prob", DataType::Float32, 1));

    StageDefinition first("tissue");
    first.writesLayer(tissue);
    ws->stages().declare(first);
    StageDefinition second("nuclei");
    second.readsLayer(tissue).writesLayer(nuclei).writesRaster(prob);
    ws->stages().declare(second);
    // A stage naming layers and rasters in every other reference member: fromWorkspace
    // declares them all, so the file accepts its record.
    StageDefinition third("refine");
    third.restrictToLayer(tissue)
        .annotatesLayer(nuclei)
        .readsLayerAsRaster("m", nuclei, LayerRasterContent::Membership)
        .readsRaster("p", prob);
    ws->stages().declare(third);

    PipelineFile f = PipelineFile::fromWorkspace(*ws);
    ASSERT_EQ(f.layerCount(), 2);
    EXPECT_STREQ(f.layerName(0), "tissue");
    EXPECT_EQ(f.layerParent(0), nullptr);
    EXPECT_EQ(f.layerKind(0), LayerKind::Semantic);
    EXPECT_STREQ(f.layerName(1), "nuclei");
    EXPECT_STREQ(f.layerParent(1), "tissue");
    ASSERT_EQ(f.rasterCount(), 1);
    EXPECT_STREQ(f.rasterAt(0).name(), "prob");
    EXPECT_EQ(f.rasterAt(0).planeType(), DataType::Float32);
    EXPECT_EQ(f.rasterAt(0).planeCount(), 1);
    ASSERT_EQ(f.stageCount(), 3);
    EXPECT_STREQ(f.stageName(0), "tissue");
    EXPECT_STREQ(f.stageName(1), "nuclei");
    EXPECT_STREQ(f.stageName(2), "refine");
    EXPECT_STREQ(f.stageRecord(2), ws->stages().configuration(ws->stages().at(2)));
    EXPECT_STREQ(f.stageRecord(0), ws->stages().configuration(ws->stages().at(0)));
    EXPECT_STREQ(f.stageRecord(1), ws->stages().configuration(ws->stages().at(1)));
    EXPECT_EQ(f.requireCount(), 0);
    EXPECT_EQ(f.exportCount(), 0);

    f.require("mylab.models");
    f.require("mylab.models");
    EXPECT_EQ(f.requireCount(), 1);
    f.addExport(ExportSpec::csv("nuclei.csv", "nuclei").measurement("area"));
    f.addExport(ExportSpec::rasterTiff("prob.tif", "prob"));
    ASSERT_EQ(f.exportCount(), 2);

    const std::string text = jsonOf(f);
    const PipelineFile again = PipelineFile::fromJson(text.c_str());
    EXPECT_TRUE(f.equals(again));
    EXPECT_EQ(jsonOf(again), text);
    ws->close();
}

TEST(PipelineFile, CopiesAMovedFromFile) {
    PipelineFile source = PipelineFile::fromJson(example().c_str());
    const PipelineFile taken(std::move(source));
    EXPECT_EQ(taken.stageCount(), 2);

    // Copy-constructing from the moved-from file gives an empty, usable file.
    PipelineFile constructed(source);  // NOLINT(bugprone-use-after-move): the case under test
    EXPECT_TRUE(constructed.equals(PipelineFile()));
    EXPECT_EQ(constructed.layerCount(), 0);
    constructed.require("mylab.models");
    EXPECT_EQ(constructed.requireCount(), 1);

    // So does copy-assigning from it, over a file that held content.
    PipelineFile assigned = PipelineFile::fromJson(example().c_str());
    assigned = source;  // NOLINT(bugprone-use-after-move): the case under test
    EXPECT_TRUE(assigned.equals(PipelineFile()));
    EXPECT_EQ(assigned.stageCount(), 0);
    assigned.require("mylab.models");
    EXPECT_EQ(assigned.requireCount(), 1);
    EXPECT_EQ(source.stageCount(), 0) << "the moved-from file still reads as empty";
}

TEST(PipelineFile, RefusesUndeclaredStageReferences) {
    // A record with every reference member: `layer` is named in the member under test, the
    // others name the declared "tissue" and "prob".
    const auto stageNaming = [](const std::string& member, const std::string& name) {
        const auto pick = [&](const char* key, const std::string& declared) {
            return member == key ? name : declared;
        };
        return "{\"stageRecordVersion\":1,\"stage\":\"refstage\",\"readsLayers\":[\"" + pick("readsLayers", "tissue") +
               "\"],\"writesLayers\":[\"" + pick("writesLayers", "tissue") + "\"],\"annotatesLayers\":[\"" +
               pick("annotatesLayers", "tissue") + "\"],\"restrictToLayer\":\"" + pick("restrictToLayer", "tissue") +
               "\",\"readsLayersAsRaster\":[{\"binding\":\"m\",\"layer\":\"" + pick("readsLayersAsRaster", "tissue") +
               "\",\"content\":\"Membership\"}],\"writesRaster\":\"" + pick("writesRaster", "prob") +
               "\",\"readsRasters\":[{\"binding\":\"p\",\"raster\":\"" + pick("readsRasters", "prob") +
               "\"}],\"dependsOn\":[],"
               "\"pipeline\":{\"specVersion\":1,\"channels\":\"all\",\"analysisLevel\":-1,\"operations\":[]}}";
    };
    const std::string tissue = "{\"name\":\"tissue\"}";
    const std::string prob = "{\"name\":\"prob\",\"type\":\"Float32\",\"planes\":1}";

    // Everything declared: accepted, and the pixel layer needs no declaration.
    EXPECT_NO_THROW(PipelineFile::fromJson(file(tissue, prob, stageNaming("", ""), "").c_str()));
    EXPECT_NO_THROW(PipelineFile::fromJson(file(tissue, prob, stageNaming("readsLayers", "pixel"), "").c_str()));

    for (const char* member : {"readsLayers", "writesLayers", "annotatesLayers", "restrictToLayer",
                               "readsLayersAsRaster", "writesRaster", "readsRasters"}) {
        const std::string message = refusal([&] {
            PipelineFile::fromJson(file(tissue, prob, stageNaming(member, "ghostname"), "").c_str());
        });
        EXPECT_NE(message.find("refstage"), std::string::npos) << member << ": " << message;
        EXPECT_NE(message.find(member), std::string::npos) << member << ": " << message;
        EXPECT_EQ(message.find("ghostname"), std::string::npos) << member << ": " << message;
    }

    // A layer name in a raster member, or a raster name in a layer member, is undeclared too.
    EXPECT_THROW(PipelineFile::fromJson(file(tissue, prob, stageNaming("writesRaster", "tissue"), "").c_str()),
                 InvalidArgumentError);
    EXPECT_THROW(PipelineFile::fromJson(file(tissue, prob, stageNaming("writesLayers", "prob"), "").c_str()),
                 InvalidArgumentError);
}
