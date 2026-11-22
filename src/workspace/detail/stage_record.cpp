// Requirements: UR-STG-4, UR-RES-11
#include <workspace/detail/stage_record.hpp>

#include <slideio/processor/core/error.hpp>
#include <slideio/processor/pipeline/operation_catalog.hpp>

#include <rapidjson/document.h>
#include <rapidjson/error/en.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <algorithm>
#include <cstring>
#include <exception>
#include <new>
#include <tuple>
#include <utility>
#include <vector>

namespace slideio::processor::detail {

namespace {

using Writer = rapidjson::Writer<rapidjson::StringBuffer>;

[[noreturn]] void fail(const std::string& what) { throw InvalidArgumentError(("stage record: " + what).c_str()); }

const char* contentName(LayerRasterContent content) {
    switch (content) {
        case LayerRasterContent::Label:
            return "Label";
        case LayerRasterContent::Membership:
            return "Membership";
        case LayerRasterContent::ObjectAnchor:
            return "ObjectAnchor";
    }
    return nullptr;
}

bool contentFromName(const std::string& name, LayerRasterContent& out) {
    for (LayerRasterContent c : {LayerRasterContent::Label, LayerRasterContent::Membership,
                                 LayerRasterContent::ObjectAnchor}) {
        if (name == contentName(c)) {
            out = c;
            return true;
        }
    }
    return false;
}

// Names the registries give, so an id they do not hold is reported against the stage.
class Namer {
public:
    Namer(const StageDefinition& definition, const LayerRegistry& layers, const RasterRegistry& rasters)
        : stage_(definition.name()), layers_(layers), rasters_(rasters) {}

    const char* layer(LayerId id) const {
        try {
            return layers_.name(id);
        } catch (const InvalidArgumentError&) {
            fail("stage '" + stage_ + "' declares a layer the store does not hold");
        }
    }
    const char* raster(RasterId id) const {
        try {
            return rasters_.name(id);
        } catch (const InvalidArgumentError&) {
            fail("stage '" + stage_ + "' declares a raster the store does not hold");
        }
    }

private:
    std::string stage_;
    const LayerRegistry& layers_;
    const RasterRegistry& rasters_;
};

void writeLayerList(Writer& w, const Namer& namer, const std::vector<LayerId>& ids) {
    w.StartArray();
    for (LayerId id : ids) {
        w.String(namer.layer(id));
    }
    w.EndArray();
}

// The record is read through a document the loader has already type-checked.
const rapidjson::Value& member(const rapidjson::Value& object, const char* key, const char* context) {
    const auto it = object.FindMember(key);
    if (it == object.MemberEnd()) {
        fail(std::string(context) + " lacks the member '" + key + "'");
    }
    return it->value;
}

std::string stringOf(const rapidjson::Value& value, const char* what) {
    if (!value.IsString()) {
        fail(std::string(what) + " is not a string");
    }
    return std::string(value.GetString(), value.GetStringLength());
}

LayerId layerNamed(const LayerRegistry& layers, const std::string& name) {
    LayerId id;
    if (!layers.find(name.c_str(), id)) {
        fail("the store holds no layer '" + name + "'");
    }
    return id;
}

RasterId rasterNamed(const RasterRegistry& rasters, const std::string& name) {
    RasterId id;
    if (!rasters.find(name.c_str(), id)) {
        fail("the store holds no raster '" + name + "'");
    }
    return id;
}

const rapidjson::Value& arrayMember(const rapidjson::Value& object, const char* key) {
    const rapidjson::Value& v = member(object, key, "the record");
    if (!v.IsArray()) {
        fail(std::string("'") + key + "' is not an array");
    }
    return v;
}

// A sorted copy of a definition's declarations, for order-insensitive comparison.
struct Declaration {
    std::vector<int32_t> explicitReads, writes, annotates;
    std::vector<std::tuple<std::string, int32_t, int32_t>> layerRasters;
    std::vector<std::pair<std::string, int32_t>> rasterReads;
    std::vector<std::string> dependencies;
};

Declaration declarationOf(const StageDefinition& d) {
    Declaration out;
    for (int32_t i = 0; i < StageDefinitionAccess::explicitReadCount(d); ++i) {
        out.explicitReads.push_back(StageDefinitionAccess::explicitReadAt(d, i).value());
    }
    for (int32_t i = 0; i < d.writtenLayerCount(); ++i) out.writes.push_back(d.writtenLayerAt(i).value());
    for (int32_t i = 0; i < d.annotatedLayerCount(); ++i) out.annotates.push_back(d.annotatedLayerAt(i).value());
    for (int32_t i = 0; i < d.rasterLayerCount(); ++i) {
        out.layerRasters.emplace_back(d.rasterBindingAt(i), d.rasterLayerAt(i).value(),
                                      static_cast<int32_t>(d.rasterContentAt(i)));
    }
    for (int32_t i = 0; i < d.readRasterCount(); ++i) {
        out.rasterReads.emplace_back(d.readRasterBindingAt(i), d.readRasterAt(i).value());
    }
    for (int32_t i = 0; i < d.explicitDependencyCount(); ++i) out.dependencies.emplace_back(d.explicitDependencyAt(i));
    std::sort(out.explicitReads.begin(), out.explicitReads.end());
    std::sort(out.writes.begin(), out.writes.end());
    std::sort(out.annotates.begin(), out.annotates.end());
    std::sort(out.layerRasters.begin(), out.layerRasters.end());
    std::sort(out.rasterReads.begin(), out.rasterReads.end());
    std::sort(out.dependencies.begin(), out.dependencies.end());
    return out;
}

} // namespace

std::string stageRecordJson(const StageDefinition& definition, const LayerRegistry& layers,
                            const RasterRegistry& rasters) {
    const Namer namer(definition, layers, rasters);
    const StageDefinition& d = definition;

    std::vector<LayerId> explicitReads, writes, annotates;
    for (int32_t i = 0; i < StageDefinitionAccess::explicitReadCount(d); ++i) {
        explicitReads.push_back(StageDefinitionAccess::explicitReadAt(d, i));
    }
    for (int32_t i = 0; i < d.writtenLayerCount(); ++i) writes.push_back(d.writtenLayerAt(i));
    for (int32_t i = 0; i < d.annotatedLayerCount(); ++i) annotates.push_back(d.annotatedLayerAt(i));

    const int32_t pipelineLength = d.pipeline().toJson(nullptr, 0);
    std::string pipelineJson(static_cast<size_t>(pipelineLength) + 1, '\0');
    d.pipeline().toJson(&pipelineJson[0], pipelineLength + 1);
    pipelineJson.resize(static_cast<size_t>(pipelineLength));

    rapidjson::StringBuffer sb;
    Writer w(sb);
    w.StartObject();
    w.Key("stageRecordVersion");
    w.Int(1);
    w.Key("stage");
    w.String(d.name());
    w.Key("readsLayers");
    writeLayerList(w, namer, explicitReads);
    w.Key("writesLayers");
    writeLayerList(w, namer, writes);
    w.Key("annotatesLayers");
    writeLayerList(w, namer, annotates);
    w.Key("restrictToLayer");
    if (d.hasLayerRestriction()) {
        w.String(namer.layer(d.layerRestriction()));
    } else {
        w.Null();
    }
    w.Key("readsLayersAsRaster");
    w.StartArray();
    for (int32_t i = 0; i < d.rasterLayerCount(); ++i) {
        w.StartObject();
        w.Key("binding");
        w.String(d.rasterBindingAt(i));
        w.Key("layer");
        w.String(namer.layer(d.rasterLayerAt(i)));
        w.Key("content");
        w.String(contentName(d.rasterContentAt(i)));
        w.EndObject();
    }
    w.EndArray();
    w.Key("writesRaster");
    if (d.writesAnyRaster()) {
        w.String(namer.raster(d.writtenRaster()));
    } else {
        w.Null();
    }
    w.Key("readsRasters");
    w.StartArray();
    for (int32_t i = 0; i < d.readRasterCount(); ++i) {
        w.StartObject();
        w.Key("binding");
        w.String(d.readRasterBindingAt(i));
        w.Key("raster");
        w.String(namer.raster(d.readRasterAt(i)));
        w.EndObject();
    }
    w.EndArray();
    w.Key("dependsOn");
    w.StartArray();
    for (int32_t i = 0; i < d.explicitDependencyCount(); ++i) w.String(d.explicitDependencyAt(i));
    w.EndArray();
    w.Key("pipeline");
    w.RawValue(pipelineJson.data(), pipelineJson.size(), rapidjson::kObjectType);
    // Operations that may vary between runs, in pipeline order; an unregistered one counts as
    // deterministic. Written only when there are some, so other records stay byte-identical.
    const OperationCatalog& catalog = OperationCatalog::instance();
    std::vector<const char*> nondeterministic;
    for (int32_t i = 0; i < d.pipeline().operationCount(); ++i) {
        const char* operation = d.pipeline().operationName(i);
        if (catalog.isRegistered(operation) && !catalog.isDeterministic(operation)) {
            nondeterministic.push_back(operation);
        }
    }
    if (!nondeterministic.empty()) {
        w.Key("nondeterministicOperations");
        w.StartArray();
        for (const char* operation : nondeterministic) w.String(operation);
        w.EndArray();
    }
    w.EndObject();
    return std::string(sb.GetString(), sb.GetSize());
}

StageDefinition stageFromRecord(const char* json, const LayerRegistry& layers, const RasterRegistry& rasters) {
    if (json == nullptr) {
        fail("the JSON text is null");
    }
    try {
        rapidjson::Document doc;
        doc.Parse<rapidjson::kParseIterativeFlag>(json);
        if (doc.HasParseError()) {
            fail(std::string("the JSON does not parse: ") + rapidjson::GetParseError_En(doc.GetParseError()) +
                 " (offset " + std::to_string(doc.GetErrorOffset()) + ")");
        }
        if (!doc.IsObject()) {
            fail("the JSON is not an object");
        }
        const rapidjson::Value& version = member(doc, "stageRecordVersion", "the record");
        if (!version.IsInt() || version.GetInt() != 1) {
            fail("stageRecordVersion is not 1");
        }
        StageDefinition def(stringOf(member(doc, "stage", "the record"), "'stage'").c_str());

        for (const rapidjson::Value& v : arrayMember(doc, "readsLayers").GetArray()) {
            def.readsLayer(layerNamed(layers, stringOf(v, "a 'readsLayers' entry")));
        }
        for (const rapidjson::Value& v : arrayMember(doc, "writesLayers").GetArray()) {
            def.writesLayer(layerNamed(layers, stringOf(v, "a 'writesLayers' entry")));
        }
        for (const rapidjson::Value& v : arrayMember(doc, "annotatesLayers").GetArray()) {
            def.annotatesLayer(layerNamed(layers, stringOf(v, "an 'annotatesLayers' entry")));
        }
        const rapidjson::Value& restriction = member(doc, "restrictToLayer", "the record");
        if (!restriction.IsNull()) {
            def.restrictToLayer(layerNamed(layers, stringOf(restriction, "'restrictToLayer'")));
        }
        for (const rapidjson::Value& v : arrayMember(doc, "readsLayersAsRaster").GetArray()) {
            if (!v.IsObject()) {
                fail("a 'readsLayersAsRaster' entry is not an object");
            }
            const std::string binding = stringOf(member(v, "binding", "a 'readsLayersAsRaster' entry"), "'binding'");
            const std::string layer = stringOf(member(v, "layer", "a 'readsLayersAsRaster' entry"), "'layer'");
            const std::string contentText = stringOf(member(v, "content", "a 'readsLayersAsRaster' entry"), "'content'");
            LayerRasterContent content;
            if (!contentFromName(contentText, content)) {
                fail("'" + contentText + "' is not a layer-raster content");
            }
            def.readsLayerAsRaster(binding.c_str(), layerNamed(layers, layer), content);
        }
        const rapidjson::Value& written = member(doc, "writesRaster", "the record");
        if (!written.IsNull()) {
            def.writesRaster(rasterNamed(rasters, stringOf(written, "'writesRaster'")));
        }
        for (const rapidjson::Value& v : arrayMember(doc, "readsRasters").GetArray()) {
            if (!v.IsObject()) {
                fail("a 'readsRasters' entry is not an object");
            }
            const std::string binding = stringOf(member(v, "binding", "a 'readsRasters' entry"), "'binding'");
            const std::string raster = stringOf(member(v, "raster", "a 'readsRasters' entry"), "'raster'");
            def.readsRaster(binding.c_str(), rasterNamed(rasters, raster));
        }
        for (const rapidjson::Value& v : arrayMember(doc, "dependsOn").GetArray()) {
            def.dependsOn(stringOf(v, "a 'dependsOn' entry").c_str());
        }
        const rapidjson::Value& pipeline = member(doc, "pipeline", "the record");
        if (!pipeline.IsObject()) {
            fail("'pipeline' is not an object");
        }
        rapidjson::StringBuffer sb;
        Writer w(sb);
        pipeline.Accept(w);
        def.pipeline(PipelineSpec::fromJson(sb.GetString()));
        return def;
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const ProcessorError&) {
        throw;
    } catch (const std::exception& e) {
        ErrorContext context;
        context.cause = e.what();
        throw InternalError("stage record: reading failed", context);
    }
}

bool sameDeclaration(const StageDefinition& a, const StageDefinition& b) {
    if (std::strcmp(a.name(), b.name()) != 0 || !a.pipeline().equals(b.pipeline())) {
        return false;
    }
    if (a.hasLayerRestriction() != b.hasLayerRestriction() ||
        (a.hasLayerRestriction() && a.layerRestriction() != b.layerRestriction())) {
        return false;
    }
    if (a.writesAnyRaster() != b.writesAnyRaster() || (a.writesAnyRaster() && a.writtenRaster() != b.writtenRaster())) {
        return false;
    }
    const Declaration x = declarationOf(a);
    const Declaration y = declarationOf(b);
    return x.explicitReads == y.explicitReads && x.writes == y.writes && x.annotates == y.annotates &&
           x.layerRasters == y.layerRasters && x.rasterReads == y.rasterReads && x.dependencies == y.dependencies;
}

} // namespace slideio::processor::detail
