// Requirements: UR-RES-8, UR-RES-13, UR-RES-15
#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/geojson.hpp>
#include <slideio/processor/core/log.hpp>
#include <slideio/processor/core/object_record.hpp>
#include <slideio/processor/core/provenance.hpp>

#include "temp_path.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <clocale>
#include <cstdint>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

using namespace slideio::processor;

static_assert(!std::is_copy_constructible<GeoJsonReader>::value, "reader is move-only");
static_assert(!std::is_copy_assignable<GeoJsonReader>::value, "reader is move-only");
static_assert(std::is_nothrow_move_constructible<GeoJsonReader>::value, "reader is movable");
static_assert(std::is_nothrow_move_assignable<GeoJsonReader>::value, "reader is movable");

namespace {

struct CaptureSink : LogSink {
    std::vector<std::pair<LogLevel, std::string>> got;
    void write(LogLevel l, const char* m) override { got.emplace_back(l, m); }
};

// Installs a capture sink and restores the process-global defaults (null sink, Warning) on exit.
class LogCapture {
public:
    LogCapture() : sink_(std::make_shared<CaptureSink>()) {
        setLogSink(sink_);
        setLogLevel(LogLevel::Warning);
    }
    LogCapture(const LogCapture&) = delete;
    LogCapture& operator=(const LogCapture&) = delete;
    ~LogCapture() {
        setLogSink(nullptr);
        setLogLevel(LogLevel::Warning);
    }
    const std::vector<std::pair<LogLevel, std::string>>& messages() const { return sink_->got; }

private:
    std::shared_ptr<CaptureSink> sink_;
};

std::string messageOf(const std::exception& e) { return e.what(); }

std::string square(int x) {
    const std::string a = std::to_string(x), b = std::to_string(x + 2);
    return "[[[" + a + ",0],[" + b + ",0],[" + b + ",2],[" + a + ",2],[" + a + ",0]]]";
}

} // namespace

TEST(GeoJsonReader, RoundTripsWriterOutput) {
    auto path = uniqueTempPath(u8"rt_ü", ".geojson");               // Review Focus 3
    TempFileGuard guard(path);
    Polygon p; Point64 o[] = {{0,0},{10,0},{10,10},{0,10}}, h[] = {{2,2},{4,2},{4,4},{2,4}};
    p.addRing(o, 4, false); p.addRing(h, 4, true);
    ObjectRecord r; r.setGeometry(p); r.setLayer(LayerId(2)); r.measurements().set("area", 96.5, MeasurementUnit::SquarePixels);
    const char* n[] = {"tumour"}; bool l[] = {false};
    { GeoJsonWriter w(path.u8string().c_str()); w.begin(Provenance()); w.writeFeature(r, "cells", n, l, 1); w.end(); }
    GeoJsonReader rd(path.u8string().c_str()); ASSERT_TRUE(rd.next());
    EXPECT_EQ(rd.geometry().ringCount(), 2); EXPECT_TRUE(rd.geometry().ringIsHole(1));
    EXPECT_EQ(rd.geometry().ringPointCount(0), 4);
    EXPECT_STREQ(rd.classification(), "tumour"); EXPECT_STREQ(rd.layerName(), "cells");
    bool sawArea = false;
    for (int32_t i = 0; i < rd.propertyCount(); ++i)
        if (std::string(rd.propertyName(i)) == "measurements.area") { sawArea = true; EXPECT_STREQ(rd.propertyValue(i), "96.5"); }
    EXPECT_TRUE(sawArea); EXPECT_FALSE(rd.next()); EXPECT_EQ(rd.featuresRead(), 1);
}

TEST(GeoJsonReader, QuPathShapedArray) {                               // Review Focus 4
    auto path = uniqueTempPath("qp", ".geojson");
    TempFileGuard guard(path);
    writeFile(path, R"([
      {"type":"Feature","id":"9f1c...","geometry":{"type":"Point","coordinates":[1,2]},"properties":{"objectType":"annotation"}},
      {"type":"Feature","geometry":{"type":"MultiPolygon","coordinates":[[[[0,0],[4,0],[4,4],[0,4],[0,0]]],[[[10,10],[12.4,10],[12,12.6],[10,10]]]]},
       "properties":{"objectType":"annotation","isLocked":false,"classification":{"name":"Tumor","color":[200,0,0]},"name":"roi 1"}}
    ])");
    GeoJsonReader rd(path.u8string().c_str()); ASSERT_TRUE(rd.next());   // Point skipped
    EXPECT_EQ(rd.geometry().ringCount(), 2); EXPECT_FALSE(rd.geometry().ringIsHole(1));
    Point64 pts[3]; rd.geometry().ringPoints(1, pts, 3); EXPECT_EQ(pts[1], (Point64{12, 10}));
    EXPECT_STREQ(rd.classification(), "Tumor"); EXPECT_STREQ(rd.layerName(), "");
    EXPECT_EQ(rd.propertyCount(), 3);                                  // objectType, isLocked, name
    EXPECT_STREQ(rd.propertyName(1), "isLocked"); EXPECT_STREQ(rd.propertyValue(1), "false");
    EXPECT_FALSE(rd.next()); EXPECT_EQ(rd.featuresRead(), 1);
}

TEST(GeoJsonReader, SingleFeatureStringClassificationAndErrors) {
    auto a = uniqueTempPath("one", ".geojson");
    TempFileGuard ga(a);
    writeFile(a, R"({"type":"Feature","geometry":{"type":"Polygon","coordinates":[[[0,0],[2,0],[2,2]]]},"properties":{"classification":"Stroma"}})");
    GeoJsonReader r(a.u8string().c_str()); ASSERT_TRUE(r.next()); EXPECT_STREQ(r.classification(), "Stroma");
    EXPECT_FALSE(r.next()); EXPECT_EQ(r.featuresRead(), 1);
    auto b = uniqueTempPath("bad", ".geojson"); TempFileGuard gb(b);
    writeFile(b, R"({"type":"FeatureCollection","features":[{"type":)");
    GeoJsonReader rb(b.u8string().c_str()); EXPECT_THROW(rb.next(), InvalidArgumentError);
    EXPECT_THROW(GeoJsonReader(uniqueTempPath("missing", ".geojson").u8string().c_str()), IoError);
}

TEST(GeoJsonReader, MissingFileMessageNamesPathAndNullPathRejected) {
    const std::string missing = uniqueTempPath("missing", ".geojson").u8string();
    try {
        GeoJsonReader r(missing.c_str());
        FAIL() << "expected IoError";
    } catch (const IoError& e) {
        EXPECT_NE(messageOf(e).find(missing), std::string::npos) << e.what();
    }
    EXPECT_THROW(GeoJsonReader(nullptr), InvalidArgumentError);
}

TEST(GeoJsonReader, ParseErrorNamesPathOffsetAndTextAndIsFinal) {
    auto path = uniqueTempPath("parse", ".geojson");
    TempFileGuard guard(path);
    writeFile(path, R"({"type":"FeatureCollection","features":[ x ]})");
    GeoJsonReader r(path.u8string().c_str());
    try {
        r.next();
        FAIL() << "expected InvalidArgumentError";
    } catch (const InvalidArgumentError& e) {
        const std::string m = e.what();
        EXPECT_NE(m.find(path.u8string()), std::string::npos) << m;
        EXPECT_NE(m.find("offset 41"), std::string::npos) << m;
        EXPECT_NE(m.find("Invalid value."), std::string::npos) << m;
    }
    EXPECT_THROW(r.next(), InvalidArgumentError);
    EXPECT_EQ(r.propertyCount(), 0);
    EXPECT_TRUE(r.geometry().isEmpty());
}

TEST(GeoJsonReader, EmptyAndScalarDocuments) {
    auto empty = uniqueTempPath("empty", ".geojson");
    TempFileGuard g1(empty);
    writeFile(empty, "");
    GeoJsonReader r1(empty.u8string().c_str());
    EXPECT_THROW(r1.next(), InvalidArgumentError);

    auto scalar = uniqueTempPath("scalar", ".geojson");
    TempFileGuard g2(scalar);
    writeFile(scalar, "42");
    GeoJsonReader r2(scalar.u8string().c_str());
    EXPECT_THROW(r2.next(), InvalidArgumentError);

    auto none = uniqueTempPath("none", ".geojson");
    TempFileGuard g3(none);
    writeFile(none, R"({"type":"FeatureCollection","features":[]})");
    GeoJsonReader r3(none.u8string().c_str());
    EXPECT_FALSE(r3.next());
    EXPECT_FALSE(r3.next());
    EXPECT_EQ(r3.featuresRead(), 0);
}

TEST(GeoJsonReader, CollectionWithKeysBeforeAndAfterFeatures) {
    auto path = uniqueTempPath("keys", ".geojson");
    TempFileGuard guard(path);
    writeFile(path, R"({"type":"FeatureCollection","bbox":[0,0,10,10],
      "provenance":{"slideId":"s","nested":{"a":[1,{"b":2}]}},
      "features":[
        {"type":"Feature","geometry":{"type":"Polygon","coordinates":)" + square(0) + R"(},"properties":{"layer":"a"}},
        {"type":"Feature","geometry":{"type":"Polygon","coordinates":)" + square(5) + R"(},"properties":{"layer":"b"}}
      ],
      "crs":{"type":"name","properties":{"name":"x"}},
      "geometry":{"type":"Polygon","coordinates":)" + square(9) + R"(},
      "properties":{"layer":"collection"}})");
    GeoJsonReader r(path.u8string().c_str());
    ASSERT_TRUE(r.next());
    EXPECT_STREQ(r.layerName(), "a");
    ASSERT_TRUE(r.next());
    EXPECT_STREQ(r.layerName(), "b");
    EXPECT_EQ(r.geometry().boundingBox().x, 5);
    EXPECT_FALSE(r.next());
    EXPECT_EQ(r.featuresRead(), 2);
}

TEST(GeoJsonReader, PropertiesSkipNestedValuesAndKeepOrder) {
    auto path = uniqueTempPath("props", ".geojson");
    TempFileGuard guard(path);
    writeFile(path, R"([{"type":"Feature","geometry":{"type":"Polygon","coordinates":)" + square(0) + R"(},
      "properties":{"a":"x\"y","list":[1,[2,{"k":3}]],"n":-1.50e3,"obj":{"deep":{"deeper":[true]}},
        "layer":7,"nil":null,"classification":null,"b":true,
        "measurements":{"area":12,"nested":{"x":1},"skip":null,"arr":[1,2],"label":"big","flag":false}}}])");
    GeoJsonReader r(path.u8string().c_str());
    ASSERT_TRUE(r.next());
    EXPECT_STREQ(r.classification(), "");
    EXPECT_STREQ(r.layerName(), "");
    const std::vector<std::pair<std::string, std::string>> want = {
        {"a", "x\"y"}, {"n", "-1.50e3"}, {"b", "true"},
        {"measurements.area", "12"}, {"measurements.label", "big"}, {"measurements.flag", "false"}};
    ASSERT_EQ(r.propertyCount(), static_cast<int32_t>(want.size()));
    for (int32_t i = 0; i < r.propertyCount(); ++i) {
        EXPECT_EQ(r.propertyName(i), want[static_cast<size_t>(i)].first);
        EXPECT_EQ(r.propertyValue(i), want[static_cast<size_t>(i)].second);
    }
    EXPECT_THROW(r.propertyName(-1), InvalidArgumentError);
    EXPECT_THROW(r.propertyValue(r.propertyCount()), InvalidArgumentError);
    EXPECT_FALSE(r.next());
}

TEST(GeoJsonReader, MeasurementsFollowTopLevelPropertiesWhateverTheirPosition) {
    auto path = uniqueTempPath("order", ".geojson");
    TempFileGuard guard(path);
    writeFile(path, R"({"type":"Feature","properties":{"measurements":{"area":1,"perimeter":4.5},"name":"x","layer":"L","k":2},
      "geometry":{"type":"Polygon","coordinates":)" + square(0) + "}}");
    GeoJsonReader r(path.u8string().c_str());
    ASSERT_TRUE(r.next());
    const std::vector<std::pair<std::string, std::string>> want = {
        {"name", "x"}, {"k", "2"}, {"measurements.area", "1"}, {"measurements.perimeter", "4.5"}};
    ASSERT_EQ(r.propertyCount(), static_cast<int32_t>(want.size()));
    for (int32_t i = 0; i < r.propertyCount(); ++i) {
        EXPECT_EQ(r.propertyName(i), want[static_cast<size_t>(i)].first);
        EXPECT_EQ(r.propertyValue(i), want[static_cast<size_t>(i)].second);
    }
}

TEST(GeoJsonReader, CollectionMembersBeforeFeaturesAreNotInherited) {
    auto path = uniqueTempPath("inherit", ".geojson");
    TempFileGuard guard(path);
    writeFile(path, R"({"type":"FeatureCollection",
      "geometry":{"type":"Polygon","coordinates":)" + square(9) + R"(},
      "properties":{"layer":"collection","classification":"C","name":"top","measurements":{"m":1}},
      "features":[{"type":"Feature","geometry":{"type":"Polygon","coordinates":)" + square(0) + R"(}}]})");
    GeoJsonReader r(path.u8string().c_str());
    ASSERT_TRUE(r.next());
    EXPECT_EQ(r.geometry().ringCount(), 1);
    EXPECT_EQ(r.geometry().boundingBox().x, 0);
    EXPECT_STREQ(r.layerName(), "");
    EXPECT_STREQ(r.classification(), "");
    EXPECT_EQ(r.propertyCount(), 0);
    EXPECT_FALSE(r.next());
    EXPECT_EQ(r.featuresRead(), 1);
}

TEST(GeoJsonReader, PolygonAndMultiPolygonRingRoles) {
    auto path = uniqueTempPath("roles", ".geojson");
    TempFileGuard guard(path);
    writeFile(path, R"({"type":"Feature","properties":{},"geometry":{"coordinates":[
        [[[0,0],[20,0],[20,20],[0,20],[0,0]],[[2,2],[4,2],[4,4],[2,2]],[[6,6],[8,6],[8,8],[6,6]]],
        [[[30,0],[40,0],[40,10],[30,0]]]],"type":"MultiPolygon"}})");
    GeoJsonReader r(path.u8string().c_str());
    ASSERT_TRUE(r.next());
    const Polygon& g = r.geometry();
    ASSERT_EQ(g.ringCount(), 4);
    EXPECT_FALSE(g.ringIsHole(0));
    EXPECT_TRUE(g.ringIsHole(1));
    EXPECT_TRUE(g.ringIsHole(2));
    EXPECT_FALSE(g.ringIsHole(3));
    EXPECT_EQ(g.ringPointCount(1), 3);
    EXPECT_EQ(r.propertyCount(), 0);
}

TEST(GeoJsonReader, UnsupportedGeometryIsSkippedWithWarning) {
    LogCapture capture;
    auto path = uniqueTempPath("skip", ".geojson");
    TempFileGuard guard(path);
    writeFile(path, R"({"type":"FeatureCollection","features":[
      {"type":"Feature","geometry":{"type":"LineString","coordinates":[[0,0],[1,1]]},"properties":{}},
      {"type":"Feature","geometry":null,"properties":{}},
      {"type":"Feature","properties":{"layer":"x"}},
      {"type":"Feature","geometry":{"type":"Polygon","coordinates":[]}},
      {"type":"Feature","geometry":{"type":"Polygon","coordinates":)" + square(0) + R"(},"properties":{"layer":"kept"}},
      {"type":"Feature","geometry":{"type":"GeometryCollection","geometries":[]}}]})");
    GeoJsonReader r(path.u8string().c_str());
    ASSERT_TRUE(r.next());
    EXPECT_STREQ(r.layerName(), "kept");
    EXPECT_FALSE(r.next());
    EXPECT_EQ(r.featuresRead(), 1);
    const auto& got = capture.messages();
    ASSERT_EQ(got.size(), 5u);
    const char* expect[][2] = {{"feature 0", "LineString"}, {"feature 1", "null"}, {"feature 2", "null"},
                               {"feature 3", "Polygon"}, {"feature 5", "GeometryCollection"}};
    for (size_t i = 0; i < got.size(); ++i) {
        EXPECT_EQ(got[i].first, LogLevel::Warning);
        EXPECT_NE(got[i].second.find(expect[i][0]), std::string::npos) << got[i].second;
        EXPECT_NE(got[i].second.find(expect[i][1]), std::string::npos) << got[i].second;
    }
}

TEST(GeoJsonReader, DegenerateRingNamesFeatureAndReaderStaysUsable) {
    auto path = uniqueTempPath("degenerate", ".geojson");
    TempFileGuard guard(path);
    writeFile(path, R"([
      {"type":"Feature","geometry":{"type":"Point","coordinates":[0,0]}},
      {"type":"Feature","geometry":{"type":"Polygon","coordinates":[[[0,0],[0,0],[0,0],[0,0]]]},"properties":{"layer":"bad"}},
      {"type":"Feature","geometry":{"type":"Polygon","coordinates":[[[0,0],[4,0],[0.2,0.3],[0,0]]]}},
      {"type":"Feature","geometry":{"type":"Polygon","coordinates":)" + square(0) + R"(},"properties":{"layer":"good"}}])");
    GeoJsonReader r(path.u8string().c_str());
    try {
        r.next();
        FAIL() << "expected InvalidArgumentError";
    } catch (const InvalidArgumentError& e) {
        EXPECT_NE(messageOf(e).find("feature 1"), std::string::npos) << e.what();
    }
    EXPECT_STREQ(r.layerName(), "");
    EXPECT_TRUE(r.geometry().isEmpty());
    try {
        r.next();                                                       // (0.2,0.3) rounds onto (0,0)
        FAIL() << "expected InvalidArgumentError";
    } catch (const InvalidArgumentError& e) {
        EXPECT_NE(messageOf(e).find("feature 2"), std::string::npos) << e.what();
    }
    ASSERT_TRUE(r.next());
    EXPECT_STREQ(r.layerName(), "good");
    EXPECT_FALSE(r.next());
    EXPECT_EQ(r.featuresRead(), 1);
}

// The reader's own non-finite check is a guard only: RapidJSON rejects a number
// that overflows a double before the handler sees it (the first case below).
TEST(GeoJsonReader, RejectsOverflowingOutOfRangeAndMalformedCoordinates) {
    {   // RapidJSON itself rejects a number that overflows a double, as a parse error.
        auto path = uniqueTempPath("huge", ".geojson");
        TempFileGuard guard(path);
        writeFile(path, R"({"type":"Feature","geometry":{"type":"Polygon","coordinates":[[[0,0],[1e400,0],[1,1]]]}})");
        GeoJsonReader r(path.u8string().c_str());
        EXPECT_THROW(r.next(), InvalidArgumentError);
    }
    const char* bad[] = {
        R"([[[0,0],[1e19,0],[1,1]]])",                     // beyond int64
        R"([[[0,0],[-9.3e18,0],[1,1]]])",                  // beyond int64
        R"([[[0,0],[2],[1,1]]])",                          // position with one number
        R"([[[0,0],[2,"x"],[1,1]]])",                      // non-number in coordinates
        R"([[[[0,0],[2,0],[1,1]]]])",                      // MultiPolygon depth for a Polygon
        R"([[[0,0],[2,0],[1,[1]]]])",                      // mixed position
    };
    for (const char* coords : bad) {
        auto path = uniqueTempPath("coords", ".geojson");
        TempFileGuard guard(path);
        writeFile(path, std::string(R"({"type":"Feature","geometry":{"type":"Polygon","coordinates":)") + coords + "}}");
        GeoJsonReader r(path.u8string().c_str());
        try {
            r.next();
            ADD_FAILURE() << "expected InvalidArgumentError for " << coords;
        } catch (const InvalidArgumentError& e) {
            EXPECT_NE(messageOf(e).find("feature 0"), std::string::npos) << e.what();
        }
    }
}

TEST(GeoJsonReader, AccessorsBeforeNextAndAfterMove) {
    auto path = uniqueTempPath("move", ".geojson");
    TempFileGuard guard(path);
    writeFile(path, R"({"type":"Feature","geometry":{"type":"Polygon","coordinates":)" + square(0) +
                        R"(},"properties":{"layer":"L","k":1}})");
    GeoJsonReader a(path.u8string().c_str());
    EXPECT_TRUE(a.geometry().isEmpty());
    EXPECT_STREQ(a.classification(), "");
    EXPECT_STREQ(a.layerName(), "");
    EXPECT_EQ(a.propertyCount(), 0);
    EXPECT_THROW(a.propertyName(0), InvalidArgumentError);
    EXPECT_EQ(a.featuresRead(), 0);
    ASSERT_TRUE(a.next());
    GeoJsonReader b(std::move(a));
    EXPECT_STREQ(b.layerName(), "L");
    EXPECT_EQ(b.propertyCount(), 1);
    EXPECT_EQ(b.featuresRead(), 1);
    EXPECT_FALSE(b.next());
    EXPECT_STREQ(b.layerName(), "");                                   // cleared by next()
    EXPECT_THROW(a.next(), InvalidArgumentError);                      // NOLINT(bugprone-use-after-move)
    EXPECT_TRUE(a.geometry().isEmpty());
    EXPECT_STREQ(a.layerName(), "");
    EXPECT_EQ(a.propertyCount(), 0);
    EXPECT_EQ(a.featuresRead(), 0);
}

namespace {

// Switches the C numeric locale to a comma-decimal one and restores the previous one on exit.
class CommaDecimalLocale {
public:
    CommaDecimalLocale() {
        const char* prev = std::setlocale(LC_NUMERIC, nullptr);
        previous_ = prev != nullptr ? prev : "C";
        for (const char* name : {"de_DE.UTF-8", "de_DE", "German_Germany.1252"}) {
            if (std::setlocale(LC_NUMERIC, name) != nullptr) {
                if (std::localeconv()->decimal_point[0] == ',') {
                    active_ = true;
                    return;
                }
                std::setlocale(LC_NUMERIC, previous_.c_str());
            }
        }
    }
    CommaDecimalLocale(const CommaDecimalLocale&) = delete;
    CommaDecimalLocale& operator=(const CommaDecimalLocale&) = delete;
    ~CommaDecimalLocale() { std::setlocale(LC_NUMERIC, previous_.c_str()); }
    bool active() const { return active_; }

private:
    std::string previous_;
    bool active_ = false;
};

} // namespace

TEST(GeoJsonReader, CoordinatesIgnoreTheCLocale) {
    CommaDecimalLocale locale;
    if (!locale.active()) {
        GTEST_SKIP() << "no comma-decimal locale available";
    }
    auto path = uniqueTempPath("locale", ".geojson");
    TempFileGuard guard(path);
    writeFile(path, R"({"type":"Feature","geometry":{"type":"Polygon","coordinates":[[[0,0],[12.6,0.4],[12,12.5e0]]]}})");
    GeoJsonReader r(path.u8string().c_str());
    ASSERT_TRUE(r.next());
    Point64 pts[3];
    r.geometry().ringPoints(0, pts, 3);
    EXPECT_EQ(pts[1], (Point64{13, 0}));
    EXPECT_EQ(pts[2], (Point64{12, 13}));
}

TEST(GeoJsonReader, LeadingUtf8BomIsSkipped) {
    auto path = uniqueTempPath("bom", ".geojson");
    TempFileGuard guard(path);
    writeFile(path, "\xEF\xBB\xBF" R"({"type":"FeatureCollection","features":[{"type":"Feature","geometry":{"type":"Polygon","coordinates":)" +
                        square(0) + R"(},"properties":{"layer":"bom"}}]})");
    GeoJsonReader r(path.u8string().c_str());
    ASSERT_TRUE(r.next());
    EXPECT_STREQ(r.layerName(), "bom");
    EXPECT_FALSE(r.next());

    auto bad = uniqueTempPath("bombad", ".geojson");             // offsets still count from the file start
    TempFileGuard guardBad(bad);
    writeFile(bad, "\xEF\xBB\xBF[ x ]");
    GeoJsonReader rb(bad.u8string().c_str());
    try {
        rb.next();
        FAIL() << "expected InvalidArgumentError";
    } catch (const InvalidArgumentError& e) {
        EXPECT_NE(messageOf(e).find("offset 5"), std::string::npos) << e.what();
    }
}

TEST(GeoJsonReader, TinyAndNegativeCoordinatesRound) {
    auto path = uniqueTempPath("tiny", ".geojson");
    TempFileGuard guard(path);
    writeFile(path, R"({"type":"Feature","geometry":{"type":"Polygon","coordinates":[[[1e-400,-0.0],[-2.5,0],[-2,-3.49E0]]]}})");
    GeoJsonReader r(path.u8string().c_str());
    ASSERT_TRUE(r.next());
    Point64 pts[3];
    r.geometry().ringPoints(0, pts, 3);
    EXPECT_EQ(pts[0], (Point64{0, 0}));
    EXPECT_EQ(pts[1], (Point64{-3, 0}));                           // llround: half away from zero
    EXPECT_EQ(pts[2], (Point64{-2, -3}));
}

TEST(GeoJsonReader, StreamsTenThousandFeatures) {
    constexpr int kCount = 10000;
    auto path = uniqueTempPath("many", ".geojson");
    TempFileGuard guard(path);
    std::string doc = R"({"type":"FeatureCollection","features":[)";
    for (int i = 0; i < kCount; ++i) {
        if (i > 0) {
            doc += ",\n";
        }
        doc += R"({"type":"Feature","geometry":{"type":"Polygon","coordinates":)" + square(i * 3) +
               R"(},"properties":{"objectType":"detection","measurements":{"i":)" + std::to_string(i) + "}}}";
    }
    doc += "]}";
    writeFile(path, doc);
    const auto start = std::chrono::steady_clock::now();
    GeoJsonReader r(path.u8string().c_str());
    int64_t seen = 0;
    while (r.next()) {
        ASSERT_EQ(r.featuresRead(), seen + 1);
        ASSERT_EQ(r.geometry().boundingBox().x, seen * 3);
        ASSERT_EQ(r.propertyCount(), 2);
        ASSERT_EQ(std::string(r.propertyValue(1)), std::to_string(seen));
        ++seen;
    }
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    EXPECT_EQ(seen, kCount);
    EXPECT_EQ(r.featuresRead(), kCount);
#ifdef NDEBUG
    EXPECT_LT(ms, 1000) << "reading took " << ms << " ms"; // Debug iterator checks are far slower
#else
    (void)ms;
#endif
}
