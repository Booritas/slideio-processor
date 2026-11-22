// Requirements: UR-API-2, UR-STG-4, UR-RES-11
#include <slideio/processor/run/pipeline_file.hpp>
#include "core/detail/diagnostics.hpp"

#include <slideio/processor/core/data_type.hpp>
#include <slideio/processor/core/error.hpp>
#include <slideio/processor/pipeline/pipeline_spec.hpp>
#include <slideio/processor/storage/layer_registry.hpp>
#include <slideio/processor/storage/store.hpp>
#include <slideio/processor/workspace/stage_registry.hpp>
#include <slideio/processor/workspace/workspace.hpp>

#include <rapidjson/document.h>
#include <rapidjson/error/en.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <new>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace slideio::processor {

namespace {

using Writer = rapidjson::Writer<rapidjson::StringBuffer>;

constexpr const char* kPixelLayer = "pixel";

struct LayerEntry {
    std::string name;
    std::string parent;  // empty: the pixel layer
    LayerKind kind = LayerKind::Semantic;
};

struct StageEntry {
    std::string name;
    std::string record;
};

[[noreturn]] void fail(const std::string& what) {
    throw InvalidArgumentError(("PipelineFile: " + what).c_str());
}

std::string foldCase(const std::string& text) {
    std::string out = text;
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

void requireIndex(int32_t index, size_t count, const char* what) {
    if (index < 0 || static_cast<size_t>(index) >= count) {
        fail(std::string(what) + " index is out of range");
    }
}

const char* kindText(LayerKind kind) { return kind == LayerKind::Primitive ? "primitive" : "semantic"; }

const char* compressionText(RasterCompression compression) {
    switch (compression) {
        case RasterCompression::None:
            return "None";
        case RasterCompression::Lzw:
            return "Lzw";
        case RasterCompression::Deflate:
            return "Deflate";
    }
    return "Deflate";
}

bool dataTypeFromText(const std::string& text, DataType& out) {
    for (int32_t i = 0; i < static_cast<int32_t>(DataType::Unknown); ++i) {
        const DataType type = static_cast<DataType>(i);
        if (text == dataTypeName(type)) {
            out = type;
            return true;
        }
    }
    return false;
}

const char* exportKindText(ExportKind kind) {
    switch (kind) {
        case ExportKind::Csv:
            return "csv";
        case ExportKind::GeoJson:
            return "geojson";
        case ExportKind::LabelMask:
            return "labelMask";
        case ExportKind::RasterTiff:
            return "rasterTiff";
    }
    return "csv";
}

bool exportKindFromText(const std::string& text, ExportKind& out) {
    for (ExportKind kind : {ExportKind::Csv, ExportKind::GeoJson, ExportKind::LabelMask, ExportKind::RasterTiff}) {
        if (text == exportKindText(kind)) {
            out = kind;
            return true;
        }
    }
    return false;
}

// A member of an object, which must exist; the context names the entry it is read from.
const rapidjson::Value& member(const rapidjson::Value& object, const char* key, const std::string& context) {
    const auto it = object.FindMember(key);
    if (it == object.MemberEnd()) {
        fail(context + " lacks the member '" + key + "'");
    }
    return it->value;
}

const rapidjson::Value* optionalMember(const rapidjson::Value& object, const char* key) {
    const auto it = object.FindMember(key);
    return it == object.MemberEnd() ? nullptr : &it->value;
}

// True when a JSON string holds a NUL ("\u0000"): read as a C string it would end early, so
// "a\u0000/b.csv" would pass for "a".
bool hasEmbeddedNul(const rapidjson::Value& s) {
    return std::strlen(s.GetString()) != static_cast<size_t>(s.GetStringLength());
}

std::string stringMember(const rapidjson::Value& object, const char* key, const std::string& context) {
    const rapidjson::Value& v = member(object, key, context);
    if (!v.IsString()) {
        fail(context + " has a '" + key + "' that is not a string");
    }
    if (hasEmbeddedNul(v)) {
        fail(context + " has a '" + key + "' that holds a NUL character");
    }
    return std::string(v.GetString(), v.GetStringLength());
}

const rapidjson::Value& arrayMember(const rapidjson::Value& object, const char* key, const std::string& context) {
    const rapidjson::Value& v = member(object, key, context);
    if (!v.IsArray()) {
        fail(context + " has a '" + key + "' that is not an array");
    }
    return v;
}

const rapidjson::Value& objectEntry(const rapidjson::Value& value, const std::string& context) {
    if (!value.IsObject()) {
        fail(context + " is not an object");
    }
    return value;
}

// An entry of the file holds only the members its format names, each once: a misspelt
// member would otherwise be dropped silently (an export would then list every measurement).
void checkMembers(const rapidjson::Value& object, const std::vector<const char*>& known, const std::string& context) {
    std::set<std::string> seen;
    for (auto it = object.MemberBegin(); it != object.MemberEnd(); ++it) {
        const std::string name(it->name.GetString(), it->name.GetStringLength());
        if (std::find_if(known.begin(), known.end(), [&](const char* k) { return name == k; }) == known.end()) {
            fail(context + " has a member this format does not define");
        }
        if (!seen.insert(name).second) {
            fail(context + " repeats the member '" + name + "'");
        }
    }
}

std::string compact(const rapidjson::Value& value) {
    rapidjson::StringBuffer sb;
    Writer w(sb);
    value.Accept(w);
    return std::string(sb.GetString(), sb.GetSize());
}

} // namespace

class PipelineFile::Impl {
public:
    std::vector<std::string> requires_;
    std::vector<LayerEntry> layers;
    std::vector<RasterDeclaration> rasters;
    std::vector<StageEntry> stages;
    std::vector<ExportSpec> exports;

    const LayerEntry* findLayer(const std::string& name) const {
        for (const LayerEntry& l : layers) {
            if (l.name == name) return &l;
        }
        return nullptr;
    }

    bool hasRaster(const std::string& name) const {
        for (const RasterDeclaration& r : rasters) {
            if (name == r.name()) return true;
        }
        return false;
    }

    void addLayer(LayerEntry entry) {
        if (entry.name.empty()) {
            fail("a layer has an empty name");
        }
        if (entry.name == kPixelLayer) {
            fail("the pixel layer is not declared in the file");
        }
        if (findLayer(entry.name) != nullptr) {
            fail("layer '" + entry.name + "' is declared twice");
        }
        if (entry.kind == LayerKind::Primitive && !entry.parent.empty()) {
            fail("primitive layer '" + entry.name + "' has a parent");
        }
        if (!entry.parent.empty() && findLayer(entry.parent) == nullptr) {
            fail("the parent of layer '" + entry.name + "' is not a layer declared before it");
        }
        layers.push_back(std::move(entry));
    }

    void addRaster(RasterDeclaration declaration) {
        const std::string name = declaration.name();
        if (name.empty()) {
            fail("a raster has an empty name");
        }
        if (hasRaster(name)) {
            fail("raster '" + name + "' is declared twice");
        }
        rasters.push_back(std::move(declaration));
    }

    // Takes a stage record by value: parsed, checked structurally and kept compact.
    void addStage(const rapidjson::Value& entry) {
        const std::string where = "a stage entry";
        const rapidjson::Value& object = objectEntry(entry, where);
        const rapidjson::Value& version = member(object, "stageRecordVersion", where);
        if (!version.IsInt() || version.GetInt() != 1) {
            fail(where + " has a stageRecordVersion other than 1");
        }
        StageEntry stage;
        stage.name = stringMember(object, "stage", where);
        if (stage.name.empty()) {
            fail(where + " has an empty 'stage' name");
        }
        const std::string named = "stage '" + stage.name + "'";
        for (const StageEntry& s : stages) {
            if (s.name == stage.name) {
                fail(named + " is declared twice");
            }
        }
        const rapidjson::Value& pipeline = member(object, "pipeline", named);
        if (!pipeline.IsObject()) {
            fail(named + " has a 'pipeline' that is not an object");
        }
        try {
            PipelineSpec::fromJson(compact(pipeline).c_str());
        } catch (const InvalidArgumentError& e) {
            ErrorContext context;
            const std::string causeText = detail::diagnosticText(e);
            context.cause = causeText.c_str();
            context.stageName = stage.name.c_str();
            throw InvalidArgumentError(("PipelineFile: the pipeline of " + named + " does not parse").c_str(), context);
        }
        // Every layer and raster the record names must be declared by the file (the pixel layer
        // is implicit): a run declares the file's layers and rasters only, so an undeclared one
        // would fail only after the workspace was created. The message names the stage and the
        // member, never the undeclared name.
        const auto requireLayer = [&](const rapidjson::Value& v, const char* key) {
            if (!v.IsString()) {
                fail(named + " has a '" + key + "' entry that is not a string");
            }
            const std::string name(v.GetString(), v.GetStringLength());
            if (name != kPixelLayer && findLayer(name) == nullptr) {
                fail(named + " names in '" + key + "' a layer the file does not declare");
            }
        };
        const auto requireRaster = [&](const rapidjson::Value& v, const char* key) {
            if (!v.IsString()) {
                fail(named + " has a '" + key + "' entry that is not a string");
            }
            const std::string name(v.GetString(), v.GetStringLength());
            if (!hasRaster(name)) {
                fail(named + " names in '" + key + "' a raster the file does not declare");
            }
        };
        const auto arrayOf = [&](const char* key) -> const rapidjson::Value* {
            const rapidjson::Value* v = optionalMember(object, key);
            if (v != nullptr && !v->IsArray()) {
                fail(named + " has a '" + key + "' that is not an array");
            }
            return v;
        };
        const auto entryMember = [&](const rapidjson::Value& v, const char* key,
                                     const char* name) -> const rapidjson::Value& {
            if (!v.IsObject()) {
                fail(named + " has a '" + key + "' entry that is not an object");
            }
            return member(v, name, named + "'s '" + key + "' entry");
        };
        for (const char* key : {"readsLayers", "annotatesLayers"}) {
            if (const rapidjson::Value* list = arrayOf(key)) {
                for (const rapidjson::Value& v : list->GetArray()) requireLayer(v, key);
            }
        }
        if (const rapidjson::Value* writes = arrayOf("writesLayers")) {
            for (const rapidjson::Value& v : writes->GetArray()) {
                requireLayer(v, "writesLayers");
            }
        }
        if (const rapidjson::Value* restriction = optionalMember(object, "restrictToLayer")) {
            if (!restriction->IsNull()) requireLayer(*restriction, "restrictToLayer");
        }
        if (const rapidjson::Value* list = arrayOf("readsLayersAsRaster")) {
            for (const rapidjson::Value& v : list->GetArray()) {
                requireLayer(entryMember(v, "readsLayersAsRaster", "layer"), "readsLayersAsRaster");
            }
        }
        if (const rapidjson::Value* raster = optionalMember(object, "writesRaster")) {
            if (!raster->IsNull()) requireRaster(*raster, "writesRaster");
        }
        if (const rapidjson::Value* list = arrayOf("readsRasters")) {
            for (const rapidjson::Value& v : list->GetArray()) {
                requireRaster(entryMember(v, "readsRasters", "raster"), "readsRasters");
            }
        }
        stage.record = compact(object);
        stages.push_back(std::move(stage));
    }

    void checkExport(const ExportSpec& spec) const {
        const std::string folded = foldCase(spec.file());
        for (const ExportSpec& other : exports) {
            if (foldCase(other.file()) == folded) {
                fail(std::string("two exports of kind ") + exportKindText(spec.kind()) +
                     " or another kind share a file name, compared without regard to case");
            }
        }
        if (spec.layer() != nullptr) {
            const std::string layer = spec.layer();
            if (findLayer(layer) == nullptr) {
                fail(std::string("an export of kind ") + exportKindText(spec.kind()) +
                     " names a layer the file does not declare");
            }
        }
        if (spec.raster() != nullptr) {
            const std::string raster = spec.raster();
            if (!hasRaster(raster)) {
                fail(std::string("an export of kind ") + exportKindText(spec.kind()) +
                     " names a raster the file does not declare");
            }
        }
    }
};

PipelineFile::PipelineFile() : impl_(std::make_unique<Impl>()) {}

PipelineFile::PipelineFile(const PipelineFile& other) : impl_(std::make_unique<Impl>(read(other.impl_))) {}

PipelineFile::PipelineFile(PipelineFile&& other) noexcept : impl_(std::move(other.impl_)) {}

PipelineFile& PipelineFile::operator=(const PipelineFile& other) {
    if (this != &other) {
        impl_ = std::make_unique<Impl>(read(other.impl_));
    }
    return *this;
}

PipelineFile& PipelineFile::operator=(PipelineFile&& other) noexcept {
    if (this != &other) {
        impl_ = std::move(other.impl_);
    }
    return *this;
}

PipelineFile::~PipelineFile() = default;

// A moved-from file has no Impl and reads as empty; the first write revives it.
const PipelineFile::Impl& PipelineFile::read(const std::unique_ptr<Impl>& impl) noexcept {
    static const Impl empty;
    return impl ? *impl : empty;
}

PipelineFile::Impl& PipelineFile::write(std::unique_ptr<Impl>& impl) {
    if (!impl) {
        impl = std::make_unique<Impl>();
    }
    return *impl;
}

PipelineFile PipelineFile::fromJson(const char* json) {
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
        const std::string top = "the file";
        objectEntry(doc, top);
        checkMembers(doc, {"formatVersion", "requires", "layers", "rasters", "stages", "exports"}, top);
        const rapidjson::Value& version = member(doc, "formatVersion", top);
        if (!version.IsInt() || version.GetInt() != 1) {
            fail("formatVersion is not 1");
        }
        PipelineFile file;
        Impl& impl = *file.impl_;

        for (const rapidjson::Value& v : arrayMember(doc, "requires", top).GetArray()) {
            if (!v.IsString() || v.GetStringLength() == 0 || hasEmbeddedNul(v)) {
                fail("a 'requires' entry is not a non-empty string without a NUL");
            }
            file.require(v.GetString());
        }
        for (const rapidjson::Value& v : arrayMember(doc, "layers", top).GetArray()) {
            const std::string where = "a layer entry";
            const rapidjson::Value& object = objectEntry(v, where);
            checkMembers(object, {"name", "parent", "kind"}, where);
            LayerEntry entry;
            entry.name = stringMember(object, "name", where);
            if (optionalMember(object, "parent") != nullptr) {
                entry.parent = stringMember(object, "parent", "layer '" + entry.name + "'");
                if (entry.parent == kPixelLayer) {
                    entry.parent.clear();
                }
            }
            if (optionalMember(object, "kind") != nullptr) {
                const std::string kind = stringMember(object, "kind", "layer '" + entry.name + "'");
                if (kind == "semantic") {
                    entry.kind = LayerKind::Semantic;
                } else if (kind == "primitive") {
                    entry.kind = LayerKind::Primitive;
                } else {
                    fail("layer '" + entry.name + "' has a kind that is neither semantic nor primitive");
                }
            }
            impl.addLayer(std::move(entry));
        }
        for (const rapidjson::Value& v : arrayMember(doc, "rasters", top).GetArray()) {
            const std::string where = "a raster entry";
            const rapidjson::Value& object = objectEntry(v, where);
            checkMembers(object, {"name", "type", "planes", "compression"}, where);
            const std::string name = stringMember(object, "name", where);
            const std::string context = "raster '" + name + "'";
            DataType type = DataType::Unknown;
            if (!dataTypeFromText(stringMember(object, "type", context), type)) {
                fail(context + " has a type that is not a sample type");
            }
            const rapidjson::Value& planes = member(object, "planes", context);
            if (!planes.IsInt() || planes.GetInt() < 1 || planes.GetInt() > 65535) {
                fail(context + " has a 'planes' that is not an integer from 1 to 65535");
            }
            RasterDeclaration declaration(name.c_str(), type, planes.GetInt());
            if (optionalMember(object, "compression") != nullptr) {
                const std::string text = stringMember(object, "compression", context);
                bool known = false;
                for (RasterCompression c : {RasterCompression::None, RasterCompression::Lzw, RasterCompression::Deflate}) {
                    if (text == compressionText(c)) {
                        declaration.compression(c);
                        known = true;
                    }
                }
                if (!known) {
                    fail(context + " has a compression that is not None, Lzw or Deflate");
                }
            }
            impl.addRaster(std::move(declaration));
        }
        for (const rapidjson::Value& v : arrayMember(doc, "stages", top).GetArray()) {
            impl.addStage(v);
        }
        for (const rapidjson::Value& v : arrayMember(doc, "exports", top).GetArray()) {
            const std::string where = "an export entry";
            const rapidjson::Value& object = objectEntry(v, where);
            checkMembers(object, {"kind", "file", "layer", "raster", "measurements"}, where);
            const std::string kindText = stringMember(object, "kind", where);
            ExportKind kind = ExportKind::Csv;
            if (!exportKindFromText(kindText, kind)) {
                fail("an export has a kind that is not csv, geojson, labelMask or rasterTiff");
            }
            const std::string context = std::string("an export of kind ") + exportKindText(kind);
            const std::string fileName = stringMember(object, "file", context);
            const bool onRaster = kind == ExportKind::RasterTiff;
            if (optionalMember(object, onRaster ? "layer" : "raster") != nullptr) {
                fail(context + (onRaster ? " names a layer instead of a raster" : " names a raster instead of a layer"));
            }
            const std::string target = stringMember(object, onRaster ? "raster" : "layer", context);
            ExportSpec spec = ExportSpec::csv("x", "x");
            switch (kind) {
                case ExportKind::Csv:
                    spec = ExportSpec::csv(fileName.c_str(), target.c_str());
                    break;
                case ExportKind::GeoJson:
                    spec = ExportSpec::geoJson(fileName.c_str(), target.c_str());
                    break;
                case ExportKind::LabelMask:
                    spec = ExportSpec::labelMask(fileName.c_str(), target.c_str());
                    break;
                case ExportKind::RasterTiff:
                    spec = ExportSpec::rasterTiff(fileName.c_str(), target.c_str());
                    break;
            }
            if (const rapidjson::Value* measurements = optionalMember(object, "measurements")) {
                if (kind != ExportKind::Csv) {
                    fail(context + " lists measurements, which only a csv export takes");
                }
                if (!measurements->IsArray()) {
                    fail(context + " has a 'measurements' that is not an array");
                }
                for (const rapidjson::Value& m : measurements->GetArray()) {
                    if (!m.IsString() || m.GetStringLength() == 0 || hasEmbeddedNul(m)) {
                        fail(context + " has a measurement that is not a non-empty string without a NUL");
                    }
                    spec.measurement(m.GetString());
                }
            }
            file.addExport(spec);
        }
        return file;
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const ProcessorError&) {
        throw;
    } catch (const std::exception& e) {
        ErrorContext context;
        const std::string causeText = detail::diagnosticText(e);
        context.cause = causeText.c_str();
        throw InternalError("PipelineFile::fromJson: reading failed", context);
    }
}

PipelineFile PipelineFile::load(const char* path) {
    if (path == nullptr) {
        fail("the path is null");
    }
    std::string text;
    {
        std::ifstream in(std::filesystem::u8path(path), std::ios::binary);
        if (!in) {
            throw IoError("PipelineFile: the pipeline file cannot be opened");
        }
        text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        if (in.bad()) {
            throw IoError("PipelineFile: the pipeline file cannot be read");
        }
    }
    return fromJson(text.c_str());
}

PipelineFile PipelineFile::fromWorkspace(Workspace& workspace) {
    PipelineFile file;
    Impl& impl = *file.impl_;
    LayerRegistry& layers = workspace.layers();
    const LayerId pixel = layers.pixelLayer();
    for (int32_t i = 0; i < layers.count(); ++i) {
        const LayerId id = layers.at(i);
        if (id == pixel) {
            continue;
        }
        LayerEntry entry;
        entry.name = layers.name(id);
        entry.kind = layers.kind(id);
        if (entry.kind == LayerKind::Semantic) {
            const LayerId parent = layers.parent(id);
            if (parent != pixel) {
                entry.parent = layers.name(parent);
            }
        }
        impl.addLayer(std::move(entry));
    }
    RasterRegistry& rasters = workspace.store()->rasters();
    for (int32_t i = 0; i < rasters.count(); ++i) {
        const RasterId id = rasters.at(i);
        // The registry cannot report a raster's compression, so none is written: the default
        // applies when the file is loaded.
        impl.addRaster(RasterDeclaration(rasters.name(id), rasters.planeType(id), rasters.planeCount(id)));
    }
    StageRegistry& stages = workspace.stages();
    for (int32_t i = 0; i < stages.count(); ++i) {
        rapidjson::Document doc;
        doc.Parse<rapidjson::kParseIterativeFlag>(stages.configuration(stages.at(i)));
        if (doc.HasParseError()) {
            fail("a stage record of the workspace does not parse");
        }
        impl.addStage(doc);
    }
    return file;
}

int32_t PipelineFile::toJson(char* buffer, int32_t capacity) const {
    const Impl& i = read(impl_);
    std::string json;
    try {
        rapidjson::StringBuffer sb;
        Writer w(sb);
        w.StartObject();
        w.Key("formatVersion");
        w.Int(1);
        w.Key("requires");
        w.StartArray();
        for (const std::string& m : i.requires_) {
            w.String(m.c_str(), static_cast<rapidjson::SizeType>(m.size()));
        }
        w.EndArray();
        w.Key("layers");
        w.StartArray();
        for (const LayerEntry& l : i.layers) {
            w.StartObject();
            w.Key("name");
            w.String(l.name.c_str(), static_cast<rapidjson::SizeType>(l.name.size()));
            if (!l.parent.empty()) {
                w.Key("parent");
                w.String(l.parent.c_str(), static_cast<rapidjson::SizeType>(l.parent.size()));
            }
            w.Key("kind");
            w.String(kindText(l.kind));
            w.EndObject();
        }
        w.EndArray();
        w.Key("rasters");
        w.StartArray();
        for (const RasterDeclaration& r : i.rasters) {
            w.StartObject();
            w.Key("name");
            w.String(r.name());
            w.Key("type");
            w.String(dataTypeName(r.planeType()));
            w.Key("planes");
            w.Int(r.planeCount());
            w.Key("compression");
            w.String(compressionText(r.compressionValue()));
            w.EndObject();
        }
        w.EndArray();
        w.Key("stages");
        w.StartArray();
        for (const StageEntry& s : i.stages) {
            w.RawValue(s.record.data(), s.record.size(), rapidjson::kObjectType);
        }
        w.EndArray();
        w.Key("exports");
        w.StartArray();
        for (const ExportSpec& e : i.exports) {
            w.StartObject();
            w.Key("kind");
            w.String(exportKindText(e.kind()));
            w.Key("file");
            w.String(e.file());
            if (e.layer() != nullptr) {
                w.Key("layer");
                w.String(e.layer());
            }
            if (e.raster() != nullptr) {
                w.Key("raster");
                w.String(e.raster());
            }
            if (e.measurementCount() > 0) {
                w.Key("measurements");
                w.StartArray();
                for (int32_t m = 0; m < e.measurementCount(); ++m) {
                    w.String(e.measurementAt(m));
                }
                w.EndArray();
            }
            w.EndObject();
        }
        w.EndArray();
        w.EndObject();
        json.assign(sb.GetString(), sb.GetSize());
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const ProcessorError&) {
        throw;
    } catch (const std::exception& e) {
        ErrorContext context;
        const std::string causeText = detail::diagnosticText(e);
        context.cause = causeText.c_str();
        throw InternalError("PipelineFile::toJson: serialisation failed", context);
    }
    const int32_t length = static_cast<int32_t>(json.size());
    if (buffer != nullptr && capacity > 0) {
        const int32_t n = length < capacity - 1 ? length : capacity - 1;
        std::memcpy(buffer, json.data(), static_cast<size_t>(n));
        buffer[n] = '\0';
    }
    return length;
}

PipelineFile& PipelineFile::require(const char* module) {
    if (module == nullptr || module[0] == '\0') {
        fail("a required module name is null or empty");
    }
    Impl& i = write(impl_);
    if (std::find(i.requires_.begin(), i.requires_.end(), module) == i.requires_.end()) {
        i.requires_.emplace_back(module);
    }
    return *this;
}

PipelineFile& PipelineFile::addExport(const ExportSpec& exportSpec) {
    Impl& i = write(impl_);
    i.checkExport(exportSpec);
    i.exports.push_back(exportSpec);
    return *this;
}

int32_t PipelineFile::requireCount() const noexcept { return static_cast<int32_t>(read(impl_).requires_.size()); }

const char* PipelineFile::requireAt(int32_t index) const {
    const Impl& i = read(impl_);
    requireIndex(index, i.requires_.size(), "requireAt");
    return i.requires_[static_cast<size_t>(index)].c_str();
}

int32_t PipelineFile::layerCount() const noexcept { return static_cast<int32_t>(read(impl_).layers.size()); }

const char* PipelineFile::layerName(int32_t index) const {
    const Impl& i = read(impl_);
    requireIndex(index, i.layers.size(), "layerName");
    return i.layers[static_cast<size_t>(index)].name.c_str();
}

const char* PipelineFile::layerParent(int32_t index) const {
    const Impl& i = read(impl_);
    requireIndex(index, i.layers.size(), "layerParent");
    const LayerEntry& l = i.layers[static_cast<size_t>(index)];
    return l.parent.empty() ? nullptr : l.parent.c_str();
}

LayerKind PipelineFile::layerKind(int32_t index) const {
    const Impl& i = read(impl_);
    requireIndex(index, i.layers.size(), "layerKind");
    return i.layers[static_cast<size_t>(index)].kind;
}

int32_t PipelineFile::rasterCount() const noexcept { return static_cast<int32_t>(read(impl_).rasters.size()); }

const RasterDeclaration& PipelineFile::rasterAt(int32_t index) const {
    const Impl& i = read(impl_);
    requireIndex(index, i.rasters.size(), "rasterAt");
    return i.rasters[static_cast<size_t>(index)];
}

int32_t PipelineFile::stageCount() const noexcept { return static_cast<int32_t>(read(impl_).stages.size()); }

const char* PipelineFile::stageName(int32_t index) const {
    const Impl& i = read(impl_);
    requireIndex(index, i.stages.size(), "stageName");
    return i.stages[static_cast<size_t>(index)].name.c_str();
}

const char* PipelineFile::stageRecord(int32_t index) const {
    const Impl& i = read(impl_);
    requireIndex(index, i.stages.size(), "stageRecord");
    return i.stages[static_cast<size_t>(index)].record.c_str();
}

int32_t PipelineFile::exportCount() const noexcept { return static_cast<int32_t>(read(impl_).exports.size()); }

const ExportSpec& PipelineFile::exportAt(int32_t index) const {
    const Impl& i = read(impl_);
    requireIndex(index, i.exports.size(), "exportAt");
    return i.exports[static_cast<size_t>(index)];
}

bool PipelineFile::equals(const PipelineFile& other) const noexcept {
    const Impl& a = read(impl_);
    const Impl& b = read(other.impl_);
    if (a.requires_ != b.requires_ || a.layers.size() != b.layers.size() || a.rasters.size() != b.rasters.size() ||
        a.stages.size() != b.stages.size() || a.exports.size() != b.exports.size()) {
        return false;
    }
    for (size_t k = 0; k < a.layers.size(); ++k) {
        if (a.layers[k].name != b.layers[k].name || a.layers[k].parent != b.layers[k].parent ||
            a.layers[k].kind != b.layers[k].kind) {
            return false;
        }
    }
    for (size_t k = 0; k < a.rasters.size(); ++k) {
        const RasterDeclaration& x = a.rasters[k];
        const RasterDeclaration& y = b.rasters[k];
        if (std::strcmp(x.name(), y.name()) != 0 || x.planeType() != y.planeType() ||
            x.planeCount() != y.planeCount() || x.compressionValue() != y.compressionValue()) {
            return false;
        }
    }
    for (size_t k = 0; k < a.stages.size(); ++k) {
        if (a.stages[k].record != b.stages[k].record) {
            return false;
        }
    }
    for (size_t k = 0; k < a.exports.size(); ++k) {
        if (!a.exports[k].equals(b.exports[k])) {
            return false;
        }
    }
    return true;
}

} // namespace slideio::processor
