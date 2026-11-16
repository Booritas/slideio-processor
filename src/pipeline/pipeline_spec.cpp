// Requirements: UR-STG-4, UR-RES-11, UR-EXE-1, UR-IMG-5
#include <slideio/processor/pipeline/pipeline_spec.hpp>

#include <slideio/processor/core/error.hpp>

#include <pipeline/detail/parameters_json.hpp>

#include <rapidjson/document.h>
#include <rapidjson/error/en.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace slideio::processor {

class PipelineSpec::Impl {
public:
    struct Entry {
        std::string name;
        OperationParameters parameters;
    };

    ChannelSelection channels = ChannelSelection::all();
    int32_t level = -1;
    bool hasRoi = false;
    Rect64 roi{0, 0, 0, 0};
    bool hasPolygon = false;
    Polygon polygon;
    std::vector<Entry> operations;
};

namespace {

[[noreturn]] void fail(const std::string& what) {
    throw InvalidArgumentError(("PipelineSpec: " + what).c_str());
}

void requireIndex(int32_t index, size_t count, const char* method) {
    if (index < 0 || static_cast<size_t>(index) >= count) {
        fail(std::string(method) + ": index " + std::to_string(index) + " is out of range [0, " +
             std::to_string(count) + ")");
    }
}

// A moved-from spec has no Impl; it reads as the default state and is revived on first write.
template <class T>
const T& orEmpty(const std::unique_ptr<T>& impl) {
    static const T instance;
    return impl ? *impl : instance;
}

template <class T>
T& writable(std::unique_ptr<T>& impl) {
    if (!impl) {
        impl = std::make_unique<T>();
    }
    return *impl;
}

bool channelsEqual(const ChannelSelection& a, const ChannelSelection& b) {
    if (a.isAll() != b.isAll()) {
        return false;
    }
    if (a.isAll()) {
        return true;
    }
    if (a.count() != b.count()) {
        return false;
    }
    for (int32_t i = 0; i < a.count(); ++i) {
        if (a.at(i) != b.at(i)) {
            return false;
        }
    }
    return true;
}

std::vector<Point64> ringOf(const Polygon& p, int32_t ring) {
    std::vector<Point64> points(static_cast<size_t>(p.ringPointCount(ring)));
    if (!points.empty()) {
        p.ringPoints(ring, points.data(), static_cast<int64_t>(points.size()));
    }
    return points;
}

bool polygonsEqual(const Polygon& a, const Polygon& b) {
    if (a.ringCount() != b.ringCount()) {
        return false;
    }
    for (int32_t r = 0; r < a.ringCount(); ++r) {
        if (a.ringIsHole(r) != b.ringIsHole(r) || a.ringPointCount(r) != b.ringPointCount(r)) {
            return false;
        }
        const std::vector<Point64> pa = ringOf(a, r);
        const std::vector<Point64> pb = ringOf(b, r);
        for (size_t i = 0; i < pa.size(); ++i) {
            if (pa[i].x != pb[i].x || pa[i].y != pb[i].y) {
                return false;
            }
        }
    }
    return true;
}

// Member access helpers for fromJson: each throws naming the member.
const rapidjson::Value& member(const rapidjson::Value& object, const char* key, const std::string& where) {
    const auto it = object.FindMember(key);
    if (it == object.MemberEnd()) {
        fail(where + ": missing member '" + key + "'");
    }
    return it->value;
}

int64_t integerOf(const rapidjson::Value& v, const std::string& where) {
    if (!v.IsInt64()) {
        fail(where + " must be an integer");
    }
    return v.GetInt64();
}

PipelineSpec readSpec(const rapidjson::Value& root) {
    if (!root.IsObject()) {
        fail("fromJson: the document must be a JSON object");
    }
    const std::string top = "fromJson";
    const rapidjson::Value& version = member(root, "specVersion", top);
    if (integerOf(version, "fromJson: 'specVersion'") != 1) {
        fail("fromJson: 'specVersion' must be 1");
    }

    PipelineSpec spec;
    const rapidjson::Value& channels = member(root, "channels", top);
    if (channels.IsString()) {
        if (std::strcmp(channels.GetString(), "all") != 0) {
            fail("fromJson: 'channels' must be \"all\" or an array of channel indices");
        }
    } else if (channels.IsArray()) {
        ChannelSelection selection;
        for (const rapidjson::Value& c : channels.GetArray()) {
            const int64_t index = integerOf(c, "fromJson: 'channels' element");
            if (index < 0 || index > std::numeric_limits<int32_t>::max()) {
                fail("fromJson: 'channels' holds an index out of range");
            }
            try {
                selection.add(static_cast<int32_t>(index));
            } catch (const InvalidArgumentError& e) {
                fail(std::string("fromJson: 'channels' is invalid: ") + e.what());
            }
        }
        spec.channels(selection);
    } else {
        fail("fromJson: 'channels' must be \"all\" or an array of channel indices");
    }

    const int64_t level = integerOf(member(root, "analysisLevel", top), "fromJson: 'analysisLevel'");
    if (level < -1 || level > std::numeric_limits<int32_t>::max()) {
        fail("fromJson: 'analysisLevel' must be -1 or a level index");
    }
    spec.analysisLevel(static_cast<int32_t>(level));

    const auto roi = root.FindMember("roi");
    if (roi != root.MemberEnd()) {
        if (!roi->value.IsArray() || roi->value.Size() != 4) {
            fail("fromJson: 'roi' must be an array of four integers [x,y,w,h]");
        }
        int64_t v[4];
        for (rapidjson::SizeType i = 0; i < 4; ++i) {
            v[i] = integerOf(roi->value[i], "fromJson: 'roi' element");
        }
        try {
            spec.restrictTo(Rect64{v[0], v[1], v[2], v[3]});
        } catch (const InvalidArgumentError& e) {
            fail(std::string("fromJson: 'roi' is invalid: ") + e.what());
        }
    }

    const auto polygon = root.FindMember("polygon");
    if (polygon != root.MemberEnd()) {
        if (!polygon->value.IsArray()) {
            fail("fromJson: 'polygon' must be an array of rings");
        }
        Polygon p;
        for (const rapidjson::Value& ring : polygon->value.GetArray()) {
            if (!ring.IsObject()) {
                fail("fromJson: 'polygon' ring must be an object");
            }
            const rapidjson::Value& hole = member(ring, "hole", "fromJson: 'polygon' ring");
            if (!hole.IsBool()) {
                fail("fromJson: 'polygon' ring member 'hole' must be a boolean");
            }
            const rapidjson::Value& points = member(ring, "points", "fromJson: 'polygon' ring");
            if (!points.IsArray()) {
                fail("fromJson: 'polygon' ring member 'points' must be an array");
            }
            std::vector<Point64> pts;
            pts.reserve(points.Size());
            for (const rapidjson::Value& pt : points.GetArray()) {
                if (!pt.IsArray() || pt.Size() != 2) {
                    fail("fromJson: 'polygon' point must be an array [x,y]");
                }
                pts.push_back(Point64{integerOf(pt[0], "fromJson: 'polygon' coordinate"),
                                      integerOf(pt[1], "fromJson: 'polygon' coordinate")});
            }
            try {
                p.addRing(pts.data(), static_cast<int64_t>(pts.size()), hole.GetBool());
            } catch (const InvalidArgumentError& e) {
                fail(std::string("fromJson: 'polygon' ring is invalid: ") + e.what());
            }
        }
        try {
            spec.restrictToPolygon(p);
        } catch (const InvalidArgumentError& e) {
            fail(std::string("fromJson: 'polygon' is invalid: ") + e.what());
        }
    }

    const rapidjson::Value& operations = member(root, "operations", top);
    if (!operations.IsArray()) {
        fail("fromJson: 'operations' must be an array");
    }
    for (const rapidjson::Value& op : operations.GetArray()) {
        if (!op.IsObject()) {
            fail("fromJson: an 'operations' element must be an object");
        }
        const rapidjson::Value& name = member(op, "name", "fromJson: operation");
        if (!name.IsString()) {
            fail("fromJson: operation member 'name' must be a string");
        }
        const std::string nameText(name.GetString(), name.GetStringLength());
        if (nameText.empty() || std::strlen(name.GetString()) != name.GetStringLength()) {
            fail("fromJson: operation member 'name' must be non-empty and free of NUL");
        }
        const rapidjson::Value& parameters = member(op, "parameters", "fromJson: operation");
        spec.operation(nameText.c_str(), detail::parametersFromJson(parameters, "PipelineSpec::fromJson"));
    }
    return spec;
}

} // namespace

PipelineSpec::PipelineSpec() : impl_(std::make_unique<Impl>()) {}

PipelineSpec::PipelineSpec(const PipelineSpec& other)
    : impl_(std::make_unique<Impl>(orEmpty(other.impl_))) {}

PipelineSpec::PipelineSpec(PipelineSpec&& other) noexcept = default;

PipelineSpec& PipelineSpec::operator=(const PipelineSpec& other) {
    if (this != &other) {
        impl_ = std::make_unique<Impl>(orEmpty(other.impl_));
    }
    return *this;
}

PipelineSpec& PipelineSpec::operator=(PipelineSpec&& other) noexcept = default;

PipelineSpec::~PipelineSpec() = default;

PipelineSpec& PipelineSpec::channels(const ChannelSelection& selection) {
    writable(impl_).channels = selection;
    return *this;
}

PipelineSpec& PipelineSpec::restrictTo(const Rect64& roi) {
    if (roi.isEmpty()) {
        fail("restrictTo: the rectangle is empty");
    }
    Impl& i = writable(impl_);
    i.roi = roi;
    i.hasRoi = true;
    return *this;
}

PipelineSpec& PipelineSpec::restrictToPolygon(const Polygon& polygon) {
    if (polygon.isEmpty()) {
        fail("restrictToPolygon: the polygon is empty");
    }
    Impl& i = writable(impl_);
    i.polygon = polygon;
    i.hasPolygon = true;
    return *this;
}

PipelineSpec& PipelineSpec::analysisLevel(int32_t level) {
    if (level < -1) {
        fail("analysisLevel: " + std::to_string(level) + " is below -1");
    }
    writable(impl_).level = level;
    return *this;
}

PipelineSpec& PipelineSpec::operation(const char* name, const OperationParameters& parameters) {
    if (name == nullptr || name[0] == '\0') {
        fail("operation: the name must be non-null and non-empty");
    }
    writable(impl_).operations.push_back(Impl::Entry{name, parameters});
    return *this;
}

PipelineSpec& PipelineSpec::then(const OperationPtr& operation) {
    if (!operation) {
        fail("then: the operation is null");
    }
    const char* name = operation->name();
    if (name == nullptr || name[0] == '\0') {
        fail("then: the operation has a null or empty name");
    }
    writable(impl_).operations.push_back(Impl::Entry{name, operation->parameters()});
    return *this;
}

int32_t PipelineSpec::operationCount() const noexcept {
    return impl_ ? static_cast<int32_t>(impl_->operations.size()) : 0;
}

const char* PipelineSpec::operationName(int32_t index) const {
    const Impl& i = orEmpty(impl_);
    requireIndex(index, i.operations.size(), "operationName");
    return i.operations[static_cast<size_t>(index)].name.c_str();
}

const OperationParameters& PipelineSpec::operationParameters(int32_t index) const {
    const Impl& i = orEmpty(impl_);
    requireIndex(index, i.operations.size(), "operationParameters");
    return i.operations[static_cast<size_t>(index)].parameters;
}

const ChannelSelection& PipelineSpec::channels() const noexcept {
    return orEmpty(impl_).channels;
}

int32_t PipelineSpec::analysisLevel() const noexcept {
    return orEmpty(impl_).level;
}

bool PipelineSpec::hasRoi() const noexcept {
    return orEmpty(impl_).hasRoi;
}

Rect64 PipelineSpec::roi() const noexcept {
    return orEmpty(impl_).roi;
}

bool PipelineSpec::hasPolygon() const noexcept {
    return orEmpty(impl_).hasPolygon;
}

const Polygon& PipelineSpec::polygon() const noexcept {
    return orEmpty(impl_).polygon;
}

int32_t PipelineSpec::toJson(char* buffer, int32_t capacity) const {
    const Impl& i = orEmpty(impl_);
    std::string json;
    try {
        rapidjson::StringBuffer sb;
        rapidjson::Writer<rapidjson::StringBuffer> w(sb);
        w.StartObject();
        w.Key("specVersion");
        w.Int(1);
        w.Key("channels");
        if (i.channels.isAll()) {
            w.String("all");
        } else {
            w.StartArray();
            for (int32_t c = 0; c < i.channels.count(); ++c) {
                w.Int(i.channels.at(c));
            }
            w.EndArray();
        }
        w.Key("analysisLevel");
        w.Int(i.level);
        if (i.hasRoi) {
            w.Key("roi");
            w.StartArray();
            w.Int64(i.roi.x);
            w.Int64(i.roi.y);
            w.Int64(i.roi.width);
            w.Int64(i.roi.height);
            w.EndArray();
        }
        if (i.hasPolygon) {
            w.Key("polygon");
            w.StartArray();
            for (int32_t r = 0; r < i.polygon.ringCount(); ++r) {
                w.StartObject();
                w.Key("hole");
                w.Bool(i.polygon.ringIsHole(r));
                w.Key("points");
                w.StartArray();
                for (const Point64& p : ringOf(i.polygon, r)) {
                    w.StartArray();
                    w.Int64(p.x);
                    w.Int64(p.y);
                    w.EndArray();
                }
                w.EndArray();
                w.EndObject();
            }
            w.EndArray();
        }
        w.Key("operations");
        w.StartArray();
        for (const Impl::Entry& e : i.operations) {
            w.StartObject();
            w.Key("name");
            w.String(e.name.c_str(), static_cast<rapidjson::SizeType>(e.name.size()));
            w.Key("parameters");
            detail::parametersToJson(e.parameters, w);
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
        context.cause = e.what();
        throw InternalError("PipelineSpec::toJson: serialisation failed", context);
    }
    const int32_t length = static_cast<int32_t>(json.size());
    if (buffer != nullptr && capacity > 0) {
        const int32_t n = length < capacity - 1 ? length : capacity - 1;
        std::memcpy(buffer, json.data(), static_cast<size_t>(n));
        buffer[n] = '\0';
    }
    return length;
}

PipelineSpec PipelineSpec::fromJson(const char* json) {
    if (json == nullptr) {
        fail("fromJson: the JSON text is null");
    }
    try {
        rapidjson::Document doc;
        // Full precision, so a double such as 0.1 reads back to the exact value written.
        doc.Parse<rapidjson::kParseFullPrecisionFlag>(json);
        if (doc.HasParseError()) {
            fail(std::string("fromJson: the JSON does not parse: ") + rapidjson::GetParseError_En(doc.GetParseError()) +
                 " (offset " + std::to_string(doc.GetErrorOffset()) + ")");
        }
        return readSpec(doc);
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const ProcessorError&) {
        throw;
    } catch (const std::exception& e) {
        ErrorContext context;
        context.cause = e.what();
        throw InternalError("PipelineSpec::fromJson: reading failed", context);
    }
}

bool PipelineSpec::equals(const PipelineSpec& other) const noexcept {
    try {
        const Impl& a = orEmpty(impl_);
        const Impl& b = orEmpty(other.impl_);
        if (a.level != b.level || a.hasRoi != b.hasRoi || a.hasPolygon != b.hasPolygon ||
            a.operations.size() != b.operations.size() || !channelsEqual(a.channels, b.channels)) {
            return false;
        }
        if (a.hasRoi && (a.roi.x != b.roi.x || a.roi.y != b.roi.y || a.roi.width != b.roi.width ||
                         a.roi.height != b.roi.height)) {
            return false;
        }
        if (a.hasPolygon && !polygonsEqual(a.polygon, b.polygon)) {
            return false;
        }
        for (size_t k = 0; k < a.operations.size(); ++k) {
            if (a.operations[k].name != b.operations[k].name ||
                !detail::parametersEqual(a.operations[k].parameters, b.operations[k].parameters)) {
                return false;
            }
        }
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace slideio::processor
