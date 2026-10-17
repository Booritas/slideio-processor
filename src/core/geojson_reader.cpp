#include <slideio/processor/core/geojson.hpp>

#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/log.hpp>

#include <rapidjson/error/en.h>
#include <rapidjson/istreamwrapper.h>
#include <rapidjson/reader.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <locale>
#include <new>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace slideio::processor {

namespace {

// Numbers arrive as their source text, so property values keep their spelling.
constexpr unsigned kParseFlags = rapidjson::kParseNumbersAsStringsFlag;
constexpr size_t kReadBufferSize = 64 * 1024;

struct Position {
    double x;
    double y;
};

// One feature as parsed, before its geometry is checked. Coordinates are kept
// as a flat list of positions plus the closing points of the arrays around
// them, because "type" may follow "coordinates" and the nesting depth that
// makes an array a ring depends on the type.
struct PendingFeature {
    struct Close {
        int depth;  // 1 is the "coordinates" array itself
        size_t end; // positions.size() when the array closed
    };

    int64_t index = 0;
    bool hasGeometry = false; // "geometry" is an object
    std::string geometryType;
    bool coordsMalformed = false;
    int positionDepth = 0; // depth of the position arrays; 0 until the first one
    std::vector<Position> positions;
    std::vector<Close> closes; // non-position arrays inside "coordinates", in closing order
    std::string classification;
    std::string layer;
    std::vector<std::string> names;  // top-level scalar properties
    std::vector<std::string> values;
    std::vector<std::string> measurementNames; // "measurements.<key>", appended after the top-level ones
    std::vector<std::string> measurementValues;

    void resetGeometry() {
        hasGeometry = false;
        geometryType.clear();
        coordsMalformed = false;
        positionDepth = 0;
        positions.clear();
        closes.clear();
    }
    void reset(int64_t featureIndex) {
        index = featureIndex;
        resetGeometry();
        classification.clear();
        layer.clear();
        names.clear();
        values.clear();
        measurementNames.clear();
        measurementValues.clear();
    }
};

// The feature the accessors report.
struct CurrentFeature {
    Polygon geometry;
    std::string classification;
    std::string layer;
    std::vector<std::string> names;
    std::vector<std::string> values;

    void clear() noexcept {
        geometry.clear();
        classification.clear();
        layer.clear();
        names.clear();
        values.clear();
    }
};

// True when the JSON number `text`, whose conversion was out of range, is too
// large rather than too small: its decimal magnitude (digits before the point,
// or minus the zeros after it, plus the exponent) is positive.
bool overflows(const char* text, size_t len) {
    size_t i = (len > 0 && text[0] == '-') ? 1 : 0;
    while (i < len && text[i] == '0') {
        ++i;
    }
    int64_t magnitude = 0;
    while (i < len && text[i] >= '0' && text[i] <= '9') {
        ++magnitude;
        ++i;
    }
    if (i < len && text[i] == '.') {
        ++i;
        if (magnitude == 0) {
            while (i < len && text[i] == '0') {
                --magnitude;
                ++i;
            }
        }
        while (i < len && text[i] >= '0' && text[i] <= '9') {
            ++i;
        }
    }
    if (i < len && (text[i] == 'e' || text[i] == 'E')) {
        ++i;
        const bool negative = i < len && text[i] == '-';
        if (i < len && (text[i] == '-' || text[i] == '+')) {
            ++i;
        }
        int64_t exponent = 0;
        while (i < len && text[i] >= '0' && text[i] <= '9') {
            exponent = std::min<int64_t>(exponent * 10 + (text[i] - '0'), 1000000);
            ++i;
        }
        magnitude += negative ? -exponent : exponent;
    }
    return magnitude > 0;
}

#if !(defined(__cpp_lib_to_chars) && __cpp_lib_to_chars >= 201611L)
// True when `text` is exactly a JSON number: -?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?
bool isJsonNumber(const char* text, size_t len) {
    size_t i = 0;
    const auto digits = [&]() {
        const size_t from = i;
        while (i < len && text[i] >= '0' && text[i] <= '9') {
            ++i;
        }
        return i - from;
    };
    if (i < len && text[i] == '-') {
        ++i;
    }
    if (i < len && text[i] == '0') {
        ++i;
    } else if (digits() == 0) {
        return false;
    }
    if (i < len && text[i] == '.') {
        ++i;
        if (digits() == 0) {
            return false;
        }
    }
    if (i < len && (text[i] == 'e' || text[i] == 'E')) {
        ++i;
        if (i < len && (text[i] == '+' || text[i] == '-')) {
            ++i;
        }
        if (digits() == 0) {
            return false;
        }
    }
    return i == len;
}
#endif

// Converts a JSON number independently of the C locale. Overflow gives
// +-infinity and underflow gives zero, as strtod would.
double parseNumber(const char* text, size_t len) {
#if defined(__cpp_lib_to_chars) && __cpp_lib_to_chars >= 201611L
    double value = 0.0;
    const std::from_chars_result r = std::from_chars(text, text + len, value);
    if (r.ec == std::errc::result_out_of_range) {
        const double sign = (len > 0 && text[0] == '-') ? -1.0 : 1.0;
        return overflows(text, len) ? sign * std::numeric_limits<double>::infinity() : 0.0;
    }
    if (r.ec != std::errc() || r.ptr != text + len) {
        return std::numeric_limits<double>::quiet_NaN(); // not a JSON number; rejected as non-finite
    }
    return value;
#else
    std::istringstream in(std::string(text, len));
    in.imbue(std::locale::classic());
    double value = 0.0;
    if (!isJsonNumber(text, len)) {
        return std::numeric_limits<double>::quiet_NaN(); // as from_chars' invalid_argument
    }
    in >> value;
    if (in.fail()) { // a well-formed number that failed is out of range
        const double sign = (len > 0 && text[0] == '-') ? -1.0 : 1.0;
        return overflows(text, len) ? sign * std::numeric_limits<double>::infinity() : 0.0;
    }
    return value;
#endif
}

// Opens the file in binary mode and skips a leading UTF-8 byte order mark,
// reporting its length in `bomSize`.
std::ifstream openInput(const std::string& path, size_t& bomSize) {
    std::ifstream in(std::filesystem::u8path(path), std::ios::binary);
    char head[3] = {};
    in.read(head, 3);
    if (in.gcount() == 3 && head[0] == '\xEF' && head[1] == '\xBB' && head[2] == '\xBF') {
        bomSize = 3;
    } else {
        in.clear();
        in.seekg(0);
    }
    return in;
}

void warnSkipped(const std::string& path, int64_t index, const std::string& reason) {
    logMessage(LogLevel::Warning, "GeoJsonReader: skipping feature %lld of %s: %s",
               static_cast<long long>(index), path.c_str(), reason.c_str());
}

// SAX handler: a state machine over the key path. Values the reader does not
// use are skipped whole by counting their nesting depth. It never throws on
// content; geometry is checked after the feature closes, outside the parser.
class Handler : public rapidjson::BaseReaderHandler<rapidjson::UTF8<>, Handler> {
public:
    enum class Ctx { TopArray, Features, Feature, Geometry, Coords, Properties, Classification, Measurements };
    enum class Scalar { Null, Bool, String, Number };

    explicit Handler(const std::string& path) : path_(path) {}

    PendingFeature pending;
    bool featureComplete = false; // set when a feature's closing brace is consumed
    bool notGeoJson = false;      // the top-level value is a scalar

    bool Null() { return scalar(Scalar::Null, "", 0); }
    bool Bool(bool b) { return b ? scalar(Scalar::Bool, "true", 4) : scalar(Scalar::Bool, "false", 5); }
    bool RawNumber(const char* s, rapidjson::SizeType len, bool) { return scalar(Scalar::Number, s, len); }
    bool String(const char* s, rapidjson::SizeType len, bool) { return scalar(Scalar::String, s, len); }
    bool Key(const char* s, rapidjson::SizeType len, bool) {
        if (skip_ == 0) {
            key_.assign(s, len);
        }
        return true;
    }
    bool StartObject() { return open(true); }
    bool EndObject(rapidjson::SizeType) { return close(); }
    bool StartArray() { return open(false); }
    bool EndArray(rapidjson::SizeType) { return close(); }

private:
    struct CoordArray {
        bool hasNumber = false;
        bool hasArray = false;
        int numbers = 0;
        double x = 0.0;
        double y = 0.0;
    };

    const std::string& path_;
    std::vector<Ctx> stack_;
    std::vector<CoordArray> coords_;
    std::string key_;
    int64_t skip_ = 0;          // depth of the value being skipped; 0 when not skipping
    int64_t nextIndex_ = 0;     // index of the next element of the feature list
    bool rootIsCollection_ = false;

    bool atRoot() const { return stack_.size() == 1; }

    void push(Ctx c) { stack_.push_back(c); }

    void notAnObject() { warnSkipped(path_, nextIndex_++, "the element is not an object"); }

    bool open(bool isObject) {
        if (skip_ > 0) {
            ++skip_;
            return true;
        }
        if (stack_.empty()) {
            if (isObject) {
                pending.reset(0); // a single Feature, unless "features" turns it into a collection
                push(Ctx::Feature);
            } else {
                push(Ctx::TopArray);
            }
            return true;
        }
        switch (stack_.back()) {
        case Ctx::TopArray:
        case Ctx::Features:
            if (isObject) {
                pending.reset(nextIndex_++);
                push(Ctx::Feature);
            } else {
                notAnObject();
                skip_ = 1;
            }
            break;
        case Ctx::Feature:
            if (atRoot() && rootIsCollection_) {
                skip_ = 1; // the collection's own members other than "features"
            } else if (isObject && key_ == "geometry") {
                pending.resetGeometry();
                pending.hasGeometry = true;
                push(Ctx::Geometry);
            } else if (isObject && key_ == "properties") {
                push(Ctx::Properties);
            } else if (!isObject && key_ == "features" && atRoot()) {
                rootIsCollection_ = true;
                push(Ctx::Features);
            } else {
                skip_ = 1;
            }
            break;
        case Ctx::Geometry:
            if (!isObject && key_ == "coordinates") {
                push(Ctx::Coords);
                coords_.emplace_back();
            } else {
                skip_ = 1;
            }
            break;
        case Ctx::Coords:
            if (isObject || coords_.back().hasNumber) {
                pending.coordsMalformed = true;
                skip_ = 1;
            } else {
                coords_.back().hasArray = true;
                push(Ctx::Coords);
                coords_.emplace_back();
            }
            break;
        case Ctx::Properties:
            if (isObject && key_ == "classification") {
                push(Ctx::Classification);
            } else if (isObject && key_ == "measurements") {
                push(Ctx::Measurements);
            } else {
                skip_ = 1;
            }
            break;
        case Ctx::Classification:
        case Ctx::Measurements:
            skip_ = 1;
            break;
        }
        return true;
    }

    bool close() {
        if (skip_ > 0) {
            --skip_;
            return true;
        }
        const Ctx ctx = stack_.back();
        stack_.pop_back();
        if (ctx == Ctx::Coords) {
            const CoordArray a = coords_.back();
            const int depth = static_cast<int>(coords_.size());
            coords_.pop_back();
            if (!a.hasNumber) {
                pending.closes.push_back({depth, pending.positions.size()});
            } else if (a.numbers < 2 || (pending.positionDepth != 0 && pending.positionDepth != depth)) {
                pending.coordsMalformed = true;
            } else {
                pending.positionDepth = depth;
                pending.positions.push_back({a.x, a.y});
            }
        } else if (ctx == Ctx::Feature) {
            // The root object closing as a collection ends the document without a feature.
            if (!(stack_.empty() && rootIsCollection_)) {
                featureComplete = true;
            }
        }
        return true;
    }

    static void property(std::vector<std::string>& names, std::vector<std::string>& values, std::string name,
                         const char* s, size_t len) {
        names.push_back(std::move(name));
        values.emplace_back(s, len);
    }

    bool scalar(Scalar kind, const char* s, size_t len) {
        if (skip_ > 0) {
            return true;
        }
        if (stack_.empty()) {
            notGeoJson = true;
            return false; // terminates the parse
        }
        switch (stack_.back()) {
        case Ctx::TopArray:
        case Ctx::Features:
            notAnObject();
            break;
        case Ctx::Feature:
            if (key_ == "geometry" && !(atRoot() && rootIsCollection_)) {
                pending.resetGeometry(); // null or a scalar: no geometry
            }
            break;
        case Ctx::Geometry:
            if (key_ == "type" && kind == Scalar::String) {
                pending.geometryType.assign(s, len);
            }
            break;
        case Ctx::Coords: {
            CoordArray& top = coords_.back();
            top.hasNumber = true;
            if (kind != Scalar::Number || top.hasArray) {
                pending.coordsMalformed = true;
            } else {
                if (top.numbers < 2) {
                    // A non-finite result is rejected when the ring is built.
                    (top.numbers == 0 ? top.x : top.y) = parseNumber(s, len);
                }
                ++top.numbers; // a third number (z) is ignored
            }
            break;
        }
        case Ctx::Properties:
            if (key_ == "classification") {
                if (kind == Scalar::String) {
                    pending.classification.assign(s, len);
                }
            } else if (key_ == "layer") {
                if (kind == Scalar::String) {
                    pending.layer.assign(s, len);
                }
            } else if (kind != Scalar::Null) {
                property(pending.names, pending.values, key_, s, len);
            }
            break;
        case Ctx::Classification:
            if (key_ == "name" && kind == Scalar::String) {
                pending.classification.assign(s, len);
            }
            break;
        case Ctx::Measurements:
            if (kind != Scalar::Null) {
                property(pending.measurementNames, pending.measurementValues, "measurements." + key_, s, len);
            }
            break;
        }
        return true;
    }
};

} // namespace

class GeoJsonReader::Impl {
public:
    explicit Impl(const char* p)
        : path(p), in(openInput(path, bomSize)), buffer(kReadBufferSize), stream(in, buffer.data(), buffer.size()),
          handler(path) {
        reader.IterativeParseInit();
    }

    std::string path;
    size_t bomSize = 0; // set by openInput; parse offsets are relative to the end of the BOM
    std::ifstream in;
    std::vector<char> buffer;
    rapidjson::IStreamWrapper stream;
    rapidjson::Reader reader;
    Handler handler;
    CurrentFeature current;
    std::vector<Point64> ring;
    std::vector<Point64> sorted;
    std::string failure; // non-empty once a parse or read error ended reading
    bool done = false;
    int64_t read = 0;

    InvalidArgumentError geometryError(const std::string& reason) const {
        return InvalidArgumentError(("GeoJsonReader: feature " + std::to_string(handler.pending.index) + " of " +
                                     path + ": " + reason)
                                        .c_str());
    }

    int64_t toCoordinate(double v) const {
        // [-2^63, 2^63): every double in it rounds to a representable int64_t.
        if (!std::isfinite(v) || v < -9223372036854775808.0 || v >= 9223372036854775808.0) {
            throw geometryError("a coordinate is not finite or does not fit a 64-bit integer");
        }
        return static_cast<int64_t>(std::llround(v));
    }

    [[noreturn]] void throwParseError() {
        const std::string offset = std::to_string(reader.GetErrorOffset() + bomSize);
        if (in.bad()) {
            failure = "GeoJsonReader: reading " + path + " failed at byte offset " + offset;
            throw IoError(failure.c_str());
        }
        failure = "GeoJsonReader: parse error in " + path + " at byte offset " + offset + ": " +
                  (handler.notGeoJson ? "the top-level value must be an object or an array"
                                      : rapidjson::GetParseError_En(reader.GetParseErrorCode()));
        throw InvalidArgumentError(failure.c_str());
    }

    // Checks the pending feature's geometry and, when it is a supported
    // polygon, makes it the current feature. False when the feature is skipped.
    bool finish() {
        PendingFeature& f = handler.pending;
        if (!f.hasGeometry) {
            warnSkipped(path, f.index, "the geometry is null or missing");
            return false;
        }
        const bool multi = f.geometryType == "MultiPolygon";
        if (!multi && f.geometryType != "Polygon") {
            warnSkipped(path, f.index,
                        "geometry type '" + (f.geometryType.empty() ? std::string("(none)") : f.geometryType) +
                            "' is not supported");
            return false;
        }
        if (f.positions.empty() && !f.coordsMalformed) {
            warnSkipped(path, f.index, "the " + f.geometryType + " has no coordinates");
            return false;
        }
        const int ringDepth = multi ? 3 : 2;
        bool malformed = f.coordsMalformed || f.positionDepth != ringDepth + 1;
        for (const PendingFeature::Close& c : f.closes) {
            malformed = malformed || c.depth > ringDepth;
        }
        if (malformed) {
            throw geometryError("the coordinates are not nested as a " + f.geometryType + " requires");
        }

        Polygon geometry;
        size_t start = 0;
        int ringInPolygon = 0;
        int ringNumber = 0;
        for (const PendingFeature::Close& c : f.closes) {
            if (c.depth == ringDepth - 1) {
                ringInPolygon = 0; // a polygon (or, for Polygon, the coordinates) closed
            }
            if (c.depth != ringDepth) {
                continue;
            }
            ring.clear();
            for (size_t k = start; k < c.end; ++k) {
                ring.push_back({toCoordinate(f.positions[k].x), toCoordinate(f.positions[k].y)});
            }
            start = c.end;
            sorted = ring;
            std::sort(sorted.begin(), sorted.end(),
                      [](Point64 a, Point64 b) { return a.x != b.x ? a.x < b.x : a.y < b.y; });
            const auto distinct = std::unique(sorted.begin(), sorted.end()) - sorted.begin();
            if (distinct < 3) {
                throw geometryError("ring " + std::to_string(ringNumber) +
                                    " has fewer than 3 distinct points after rounding");
            }
            try {
                geometry.addRing(ring.data(), static_cast<int64_t>(ring.size()), ringInPolygon > 0);
            } catch (const InvalidArgumentError& e) {
                throw geometryError(e.what());
            }
            ++ringInPolygon;
            ++ringNumber;
        }

        current.geometry = std::move(geometry);
        current.classification.swap(f.classification);
        current.layer.swap(f.layer);
        // Top-level properties first, then measurements, whatever their document order.
        std::move(f.measurementNames.begin(), f.measurementNames.end(), std::back_inserter(f.names));
        std::move(f.measurementValues.begin(), f.measurementValues.end(), std::back_inserter(f.values));
        current.names.swap(f.names);
        current.values.swap(f.values);
        return true;
    }
};

GeoJsonReader::GeoJsonReader(const char* path) {
    if (path == nullptr) {
        throw InvalidArgumentError("GeoJsonReader: path must not be null");
    }
    const std::string message = std::string("GeoJsonReader: cannot open ") + path;
    std::unique_ptr<Impl> impl;
    try {
        impl = std::make_unique<Impl>(path);
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::exception& e) {
        ErrorContext ctx;
        ctx.cause = e.what();
        throw IoError(message.c_str(), ctx);
    }
    if (!impl->in.is_open()) {
        throw IoError(message.c_str());
    }
    impl_ = std::move(impl);
}

GeoJsonReader::GeoJsonReader(GeoJsonReader&& other) noexcept = default;
GeoJsonReader& GeoJsonReader::operator=(GeoJsonReader&& other) noexcept = default;
GeoJsonReader::~GeoJsonReader() = default;

bool GeoJsonReader::next() {
    if (!impl_) {
        throw InvalidArgumentError("GeoJsonReader::next: the reader was moved from");
    }
    Impl& d = *impl_;
    d.current.clear();
    if (!d.failure.empty()) {
        throw InvalidArgumentError(("GeoJsonReader::next: reading already failed: " + d.failure).c_str());
    }
    while (!d.done) {
        d.handler.featureComplete = false;
        try {
            while (!d.handler.featureComplete && !d.reader.IterativeParseComplete()) {
                if (!d.reader.IterativeParseNext<kParseFlags>(d.stream, d.handler)) {
                    d.throwParseError();
                }
            }
        } catch (const ProcessorError&) {
            throw;
        } catch (const std::bad_alloc&) {
            d.failure = "GeoJsonReader: out of memory reading " + d.path;
            throw;
        } catch (const std::ios_base::failure& e) {
            d.failure = "GeoJsonReader: reading " + d.path + " failed";
            ErrorContext ctx;
            ctx.cause = e.what();
            throw IoError(d.failure.c_str(), ctx);
        } catch (const std::exception& e) {
            d.failure = "GeoJsonReader: reading " + d.path + " failed";
            ErrorContext ctx;
            ctx.cause = e.what();
            throw InternalError(d.failure.c_str(), ctx);
        }
        if (!d.handler.featureComplete) {
            d.done = true;
            break;
        }
        if (d.finish()) {
            ++d.read;
            return true;
        }
    }
    return false;
}

const Polygon& GeoJsonReader::geometry() const noexcept {
    static const Polygon kEmpty;
    return impl_ ? impl_->current.geometry : kEmpty;
}

const char* GeoJsonReader::classification() const noexcept {
    return impl_ ? impl_->current.classification.c_str() : "";
}

const char* GeoJsonReader::layerName() const noexcept { return impl_ ? impl_->current.layer.c_str() : ""; }

int32_t GeoJsonReader::propertyCount() const noexcept {
    return impl_ ? static_cast<int32_t>(impl_->current.names.size()) : 0;
}

const char* GeoJsonReader::propertyName(int32_t index) const {
    if (index < 0 || index >= propertyCount()) {
        throw InvalidArgumentError("GeoJsonReader::propertyName: index out of range");
    }
    return impl_->current.names[static_cast<size_t>(index)].c_str();
}

const char* GeoJsonReader::propertyValue(int32_t index) const {
    if (index < 0 || index >= propertyCount()) {
        throw InvalidArgumentError("GeoJsonReader::propertyValue: index out of range");
    }
    return impl_->current.values[static_cast<size_t>(index)].c_str();
}

int64_t GeoJsonReader::featuresRead() const noexcept { return impl_ ? impl_->read : 0; }

} // namespace slideio::processor
