#include <slideio/processor/core/geojson.hpp>

#include <slideio/processor/core/error.hpp>

#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <algorithm>
#include <cmath>
#include <exception>
#include <filesystem>
#include <fstream>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace slideio::processor {

namespace {

using JsonWriter = rapidjson::Writer<rapidjson::StringBuffer>;

// True when p lies on the closed segment a-b.
bool onSegment(Point64 p, Point64 a, Point64 b) {
    const double cross = (static_cast<double>(b.x) - static_cast<double>(a.x)) *
                             (static_cast<double>(p.y) - static_cast<double>(a.y)) -
                         (static_cast<double>(b.y) - static_cast<double>(a.y)) *
                             (static_cast<double>(p.x) - static_cast<double>(a.x));
    if (cross != 0.0) {
        return false;
    }
    return p.x >= std::min(a.x, b.x) && p.x <= std::max(a.x, b.x) && p.y >= std::min(a.y, b.y) &&
           p.y <= std::max(a.y, b.y);
}

// Even-odd test of the exact vertex p against one ring, with the crossing rule
// Polygon::contains uses. A point on the boundary counts as inside.
bool ringContainsVertex(const std::vector<Point64>& ring, Point64 p) {
    const size_t n = ring.size();
    bool inside = false;
    for (size_t k = 0; k < n; ++k) {
        const Point64 a = ring[k];
        const Point64 b = ring[k + 1 == n ? 0 : k + 1];
        if (onSegment(p, a, b)) {
            return true;
        }
        if ((a.y <= p.y) == (b.y <= p.y)) {
            continue;
        }
        const double dy = static_cast<double>(b.y) - static_cast<double>(a.y);
        const double lhs = (static_cast<double>(p.x) - static_cast<double>(a.x)) * dy;
        const double rhs = (static_cast<double>(p.y) - static_cast<double>(a.y)) *
                           (static_cast<double>(b.x) - static_cast<double>(a.x));
        if (dy > 0 ? lhs < rhs : lhs > rhs) {
            inside = !inside;
        }
    }
    return inside;
}

// Writes a ring closed: its vertices, then the first one again.
void writeRing(JsonWriter& w, const std::vector<Point64>& ring) {
    w.StartArray();
    for (size_t i = 0; i <= ring.size(); ++i) {
        const Point64& p = ring[i == ring.size() ? 0 : i];
        w.StartArray();
        w.Int64(p.x);
        w.Int64(p.y);
        w.EndArray();
    }
    w.EndArray();
}

std::vector<Point64> readRing(const Polygon& g, int32_t ring) {
    const int64_t n = g.ringPointCount(ring);
    std::vector<Point64> pts(static_cast<size_t>(n));
    g.ringPoints(ring, pts.data(), n);
    return pts;
}

void writeGeometry(JsonWriter& w, const Polygon& g) {
    std::vector<std::vector<Point64>> rings;
    std::vector<size_t> outers;
    std::vector<size_t> holes;
    for (int32_t r = 0; r < g.ringCount(); ++r) {
        rings.push_back(readRing(g, r));
        (g.ringIsHole(r) ? holes : outers).push_back(static_cast<size_t>(r));
    }
    // R12: each hole goes to the first outer ring containing its first vertex, else the first outer.
    std::vector<std::vector<size_t>> owned(outers.size());
    for (size_t h : holes) {
        size_t owner = 0;
        for (size_t o = 0; o < outers.size(); ++o) {
            if (ringContainsVertex(rings[outers[o]], rings[h][0])) {
                owner = o;
                break;
            }
        }
        owned[owner].push_back(h);
    }
    const bool multi = outers.size() != 1;
    w.StartObject();
    w.Key("type");
    w.String(multi ? "MultiPolygon" : "Polygon");
    w.Key("coordinates");
    w.StartArray();
    for (size_t o = 0; o < outers.size(); ++o) {
        if (multi) {
            w.StartArray();
        }
        writeRing(w, rings[outers[o]]);
        for (size_t h : owned[o]) {
            writeRing(w, rings[h]);
        }
        if (multi) {
            w.EndArray();
        }
    }
    w.EndArray();
    w.EndObject();
}

} // namespace

class GeoJsonWriter::Impl {
public:
    enum class State { Opened, Begun, Ended };

    std::ofstream out;
    std::string path;
    State state = State::Opened;
    int64_t written = 0;

    void raw(const char* data, size_t size) { out.write(data, static_cast<std::streamsize>(size)); }
    void raw(const char* text) { raw(text, std::char_traits<char>::length(text)); }
    IoError failure(const char* what, const char* cause = nullptr) const {
        ErrorContext ctx;
        ctx.cause = cause;
        return IoError((std::string("GeoJsonWriter: ") + what + " failed for " + path).c_str(), ctx);
    }
    void checkStream(const char* what) const {
        if (!out) {
            throw failure(what);
        }
    }
};

GeoJsonWriter::GeoJsonWriter(const char* path) {
    if (path == nullptr) {
        throw InvalidArgumentError("GeoJsonWriter: path must not be null");
    }
    auto impl = std::make_unique<Impl>();
    impl->path = path;
    try {
        impl->out.open(std::filesystem::u8path(impl->path), std::ios::binary | std::ios::trunc);
    } catch (const std::bad_alloc&) {
        throw; // C-6: never wrapped
    } catch (const std::exception& e) {
        throw impl->failure("open", e.what());
    }
    if (!impl->out.is_open()) {
        throw impl->failure("open");
    }
    impl_ = std::move(impl);
}

GeoJsonWriter::GeoJsonWriter(GeoJsonWriter&& other) noexcept = default;
GeoJsonWriter& GeoJsonWriter::operator=(GeoJsonWriter&& other) noexcept = default;
GeoJsonWriter::~GeoJsonWriter() = default;

void GeoJsonWriter::begin(const Provenance& provenance) {
    if (!impl_ || impl_->state != Impl::State::Opened) {
        throw InvalidArgumentError("GeoJsonWriter::begin: already begun, ended, or moved from");
    }
    const int32_t len = provenance.toJson(nullptr, 0);
    std::vector<char> json(static_cast<size_t>(len) + 1);
    provenance.toJson(json.data(), static_cast<int32_t>(json.size()));
    impl_->state = Impl::State::Begun;
    try {
        impl_->raw("{\"type\":\"FeatureCollection\",\"provenance\":");
        impl_->raw(json.data(), static_cast<size_t>(len));
        impl_->raw(",\"features\":[\n");
    } catch (const std::bad_alloc&) {
        throw; // C-6: never wrapped
    } catch (const std::exception& e) {
        throw impl_->failure("write", e.what());
    }
    impl_->checkStream("write");
}

void GeoJsonWriter::writeFeature(const ObjectRecord& record, const char* layerName,
                                 const char* const* labelNames, const bool* labelIsWorkspaceLocal,
                                 int32_t labelCount) {
    if (!impl_ || impl_->state != Impl::State::Begun) {
        throw InvalidArgumentError("GeoJsonWriter::writeFeature: not begun, already ended, or moved from");
    }
    if (layerName == nullptr) {
        throw InvalidArgumentError("GeoJsonWriter::writeFeature: layerName must not be null");
    }
    if (labelCount < 0) {
        throw InvalidArgumentError("GeoJsonWriter::writeFeature: labelCount must not be negative");
    }
    if (labelCount > 0) {
        if (labelNames == nullptr || labelIsWorkspaceLocal == nullptr) {
            throw InvalidArgumentError("GeoJsonWriter::writeFeature: label arrays must not be null");
        }
        for (int32_t i = 0; i < labelCount; ++i) {
            if (labelNames[i] == nullptr) {
                throw InvalidArgumentError("GeoJsonWriter::writeFeature: label names must not be null");
            }
        }
    }
    const Polygon& geometry = record.geometry();
    if (geometry.isEmpty()) {
        throw InvalidArgumentError("GeoJsonWriter::writeFeature: geometry must not be empty");
    }
    const MeasurementSet& ms = record.measurements();
    for (int32_t i = 0; i < ms.count(); ++i) {
        if (!std::isfinite(ms.at(i).value())) {
            throw InvalidArgumentError("GeoJsonWriter::writeFeature: measurement values must be finite");
        }
    }

    // Build the whole feature first, so a failure here writes nothing.
    rapidjson::StringBuffer buf;
    JsonWriter w(buf);
    w.StartObject();
    w.Key("type");
    w.String("Feature");
    w.Key("geometry");
    writeGeometry(w, geometry);
    w.Key("properties");
    w.StartObject();
    w.Key("objectId");
    const ObjectId oid = record.id();
    std::vector<char> id(static_cast<size_t>(oid.toString(nullptr, 0)) + 1);
    oid.toString(id.data(), static_cast<int32_t>(id.size()));
    w.String(id.data());
    w.Key("objectType");
    w.String("detection");
    w.Key("layer");
    w.String(layerName);
    if (record.stage().isValid()) {
        w.Key("stageId");
        w.Int(record.stage().value());
    }
    if (labelCount > 0) {
        w.Key("classification");
        w.StartObject();
        w.Key("name");
        w.String(labelNames[0]);
        w.EndObject();
    }
    w.Key("labels");
    w.StartArray();
    for (int32_t i = 0; i < labelCount; ++i) {
        w.StartObject();
        w.Key("name");
        w.String(labelNames[i]);
        w.Key("workspaceLocal");
        w.Bool(labelIsWorkspaceLocal[i]);
        w.EndObject();
    }
    w.EndArray();
    w.Key("measurements");
    w.StartObject();
    for (int32_t i = 0; i < ms.count(); ++i) {
        const Measurement m = ms.at(i);
        w.Key(m.name());
        w.Double(m.value());
    }
    w.EndObject();
    w.EndObject();
    w.EndObject();

    try {
        if (impl_->written > 0) {
            impl_->raw(",\n");
        }
        impl_->raw(buf.GetString(), buf.GetSize());
    } catch (const std::bad_alloc&) {
        throw; // C-6: never wrapped
    } catch (const std::exception& e) {
        throw impl_->failure("write", e.what());
    }
    ++impl_->written;
    impl_->checkStream("write");
}

void GeoJsonWriter::end() {
    if (!impl_ || impl_->state != Impl::State::Begun) {
        throw InvalidArgumentError("GeoJsonWriter::end: not begun, already ended, or moved from");
    }
    impl_->state = Impl::State::Ended;
    try {
        impl_->raw("\n]}\n");
        impl_->out.flush();
    } catch (const std::bad_alloc&) {
        throw; // C-6: never wrapped
    } catch (const std::exception& e) {
        throw impl_->failure("write", e.what());
    }
    impl_->checkStream("write");
    impl_->out.close();
    impl_->checkStream("close");
}

int64_t GeoJsonWriter::featuresWritten() const noexcept { return impl_ ? impl_->written : 0; }

} // namespace slideio::processor
