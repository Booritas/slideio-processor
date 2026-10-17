// Requirements: UR-RES-8, UR-RES-11, UR-RES-15, UR-RES-18, UR-QA-3
#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/geojson.hpp>
#include <slideio/processor/core/object_record.hpp>
#include <slideio/processor/core/provenance.hpp>

#include "temp_path.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

using namespace slideio::processor;

static_assert(!std::is_copy_constructible<GeoJsonWriter>::value, "writer is move-only");
static_assert(!std::is_copy_assignable<GeoJsonWriter>::value, "writer is move-only");
static_assert(std::is_nothrow_move_constructible<GeoJsonWriter>::value, "writer is movable");

namespace {

ObjectRecord triangle(int32_t layer) {
    Polygon p;
    Point64 a[] = {{0, 0}, {4, 0}, {4, 4}};
    p.addRing(a, 3, false);
    ObjectRecord r;
    r.setGeometry(p);
    r.setLayer(LayerId(layer));
    return r;
}

// Writes one record with no labels and returns the document.
std::string writeOne(const ObjectRecord& r, const char* stem) {
    auto path = uniqueTempPath(stem, ".geojson");
    TempFileGuard guard(path);
    {
        GeoJsonWriter w(path.u8string().c_str());
        w.begin(Provenance());
        w.writeFeature(r, "layer", nullptr, nullptr, 0);
        w.end();
    }
    return readFile(path);
}

} // namespace

TEST(GeoJsonWriter, ExactDocumentForOneFeature) {
    auto path = uniqueTempPath("w", ".geojson");
    TempFileGuard guard(path);
    Provenance prov;
    prov.setSlideId("s1");
    ObjectRecord r;
    Point64 v[] = {{1, 2}, {5, 2}, {5, 6}};
    Polygon p;
    p.addRing(v, 3, false);
    r.setGeometry(p);
    r.setLayer(LayerId(3));
    r.measurements().set("area", 8, MeasurementUnit::SquarePixels);
    const char* names[] = {"tumour", "mine"};
    bool local[] = {false, true};
    {
        GeoJsonWriter w(path.u8string().c_str());
        w.begin(prov);
        w.writeFeature(r, "nuclei", names, local, 2);
        w.end();
        EXPECT_EQ(w.featuresWritten(), 1);
    }
    std::vector<char> pj(prov.toJson(nullptr, 0) + 1);
    prov.toJson(pj.data(), static_cast<int32_t>(pj.size()));
    std::string expect =
        std::string("{\"type\":\"FeatureCollection\",\"provenance\":") + pj.data() +
        ",\"features\":[\n{\"type\":\"Feature\",\"geometry\":{\"type\":\"Polygon\",\"coordinates\":[[[1,2],[5,2],[5,6],[1,2]]]},"
        "\"properties\":{\"objectId\":\"3:1:2\",\"objectType\":\"detection\",\"layer\":\"nuclei\","
        "\"classification\":{\"name\":\"tumour\"},\"labels\":[{\"name\":\"tumour\",\"workspaceLocal\":false},"
        "{\"name\":\"mine\",\"workspaceLocal\":true}],\"measurements\":{\"area\":8.0}}}\n]}\n";
    EXPECT_EQ(readFile(path), expect);
}

TEST(GeoJsonWriter, TwoOutersBecomeMultiPolygonWithHoleAssigned) {
    Polygon p;
    Point64 a[] = {{0, 0}, {10, 0}, {10, 10}, {0, 10}}, b[] = {{20, 0}, {30, 0}, {30, 10}, {20, 10}},
            h[] = {{22, 2}, {24, 2}, {24, 4}, {22, 4}};
    p.addRing(a, 4, false);
    p.addRing(b, 4, false);
    p.addRing(h, 4, true);
    ObjectRecord r;
    r.setGeometry(p);
    r.setLayer(LayerId(1));
    const std::string s = writeOne(r, "mp");
    EXPECT_NE(s.find("\"MultiPolygon\",\"coordinates\":[[[[0,0],[10,0],[10,10],[0,10],[0,0]]],"
                     "[[[20,0],[30,0],[30,10],[20,10],[20,0]],[[22,2],[24,2],[24,4],[22,4],[22,2]]]]"),
              std::string::npos)
        << s;
    EXPECT_EQ(s.find("classification"), std::string::npos);
}

TEST(GeoJsonWriter, HoleOutsideEveryOuterGoesToFirstOuter) {
    Polygon p;
    Point64 a[] = {{0, 0}, {10, 0}, {10, 10}, {0, 10}}, b[] = {{20, 0}, {30, 0}, {30, 10}, {20, 10}},
            h[] = {{50, 50}, {52, 50}, {52, 52}};
    p.addRing(a, 4, false);
    p.addRing(b, 4, false);
    p.addRing(h, 3, true);
    ObjectRecord r;
    r.setGeometry(p);
    r.setLayer(LayerId(1));
    const std::string s = writeOne(r, "mp2");
    EXPECT_NE(s.find("[[[[0,0],[10,0],[10,10],[0,10],[0,0]],[[50,50],[52,50],[52,52],[50,50]]],"
                     "[[[20,0],[30,0],[30,10],[20,10],[20,0]]]]"),
              std::string::npos)
        << s;
}

TEST(GeoJsonWriter, HoleInsideSecondOuterIsAssignedToIt) {
    Polygon p;
    Point64 a[] = {{0, 0}, {10, 0}, {10, 10}, {0, 10}}, b[] = {{20, 0}, {30, 0}, {30, 10}, {20, 10}},
            h[] = {{22, 2}, {24, 2}, {24, 4}};
    p.addRing(a, 4, false);
    p.addRing(h, 3, true);
    p.addRing(b, 4, false);
    ObjectRecord r;
    r.setGeometry(p);
    r.setLayer(LayerId(1));
    const std::string s = writeOne(r, "mp3");
    EXPECT_NE(s.find("[[[[0,0],[10,0],[10,10],[0,10],[0,0]]],"
                     "[[[20,0],[30,0],[30,10],[20,10],[20,0]],[[22,2],[24,2],[24,4],[22,2]]]]"),
              std::string::npos)
        << s;
}

TEST(GeoJsonWriter, HoleVertexOnOuterBoundaryCountsAsInside) {
    Polygon p;
    Point64 a[] = {{0, 0}, {10, 0}, {10, 10}, {0, 10}}, b[] = {{20, 0}, {30, 0}, {30, 10}, {20, 10}},
            h[] = {{20, 5}, {24, 4}, {24, 6}};
    p.addRing(a, 4, false);
    p.addRing(b, 4, false);
    p.addRing(h, 3, true);
    ObjectRecord r;
    r.setGeometry(p);
    r.setLayer(LayerId(1));
    const std::string s = writeOne(r, "mp4");
    EXPECT_NE(s.find("[[[20,0],[30,0],[30,10],[20,10],[20,0]],[[20,5],[24,4],[24,6],[20,5]]]]"), std::string::npos)
        << s;
}

TEST(GeoJsonWriter, SingleOuterWithHoleIsAPolygon) {
    Polygon p;
    Point64 a[] = {{0, 0}, {10, 0}, {10, 10}, {0, 10}}, h[] = {{2, 2}, {4, 2}, {4, 4}};
    p.addRing(a, 4, false);
    p.addRing(h, 3, true);
    ObjectRecord r;
    r.setGeometry(p);
    r.setLayer(LayerId(1));
    const std::string s = writeOne(r, "ph");
    EXPECT_NE(s.find("\"Polygon\",\"coordinates\":[[[0,0],[10,0],[10,10],[0,10],[0,0]],"
                     "[[2,2],[4,2],[4,4],[2,2]]]}"),
              std::string::npos)
        << s;
}

TEST(GeoJsonWriter, FeaturesAreOnePerLineAndCounted) {
    const ObjectRecord r = triangle(2);
    auto path = uniqueTempPath("n", ".geojson");
    TempFileGuard guard(path);
    GeoJsonWriter w(path.u8string().c_str());
    w.begin(Provenance());
    w.writeFeature(r, "a", nullptr, nullptr, 0);
    w.writeFeature(r, "a", nullptr, nullptr, 0);
    w.writeFeature(r, "a", nullptr, nullptr, 0);
    EXPECT_EQ(w.featuresWritten(), 3);
    w.end();
    const std::string s = readFile(path);
    int lines = 0;
    for (char c : s) {
        lines += c == '\n' ? 1 : 0;
    }
    // header line, three features, closing line
    EXPECT_EQ(lines, 1 + 3 + 1) << s;
    EXPECT_NE(s.find("}}},\n{\"type\":\"Feature\""), std::string::npos) << s;
    EXPECT_EQ(s.rfind("}}}\n]}\n"), s.size() - 7) << s;
}

TEST(GeoJsonWriter, EmptyCollection) {
    auto path = uniqueTempPath("e", ".geojson");
    TempFileGuard guard(path);
    {
        GeoJsonWriter w(path.u8string().c_str());
        w.begin(Provenance());
        w.end();
        EXPECT_EQ(w.featuresWritten(), 0);
    }
    EXPECT_NE(readFile(path).find(",\"features\":[\n\n]}\n"), std::string::npos);
}

TEST(GeoJsonWriter, MeasurementsFollowSetOrderAndStageIsOmittedWhenInvalid) {
    ObjectRecord r = triangle(2);
    r.measurements().set("b", 1.5, MeasurementUnit::Pixels);
    r.measurements().set("a", -2, MeasurementUnit::Dimensionless);
    const std::string s = writeOne(r, "m");
    EXPECT_NE(s.find("\"measurements\":{\"b\":1.5,\"a\":-2.0}"), std::string::npos) << s;
    EXPECT_NE(s.find("\"labels\":[]"), std::string::npos) << s;
    EXPECT_EQ(s.find("stageId"), std::string::npos) << s;
}

TEST(GeoJsonWriter, ErrorsAndUtf8Path) {
    auto path = uniqueTempPath(u8"tést_日本", ".geojson");
    TempFileGuard guard(path);
    GeoJsonWriter w(path.u8string().c_str());
    ObjectRecord r;
    EXPECT_THROW(w.writeFeature(r, "x", nullptr, nullptr, 0), InvalidArgumentError);
    w.begin(Provenance());
    EXPECT_THROW(w.begin(Provenance()), InvalidArgumentError);
    EXPECT_THROW(w.writeFeature(r, "x", nullptr, nullptr, 0), InvalidArgumentError); // empty geometry
    w.end();
    EXPECT_TRUE(std::filesystem::exists(path));
    EXPECT_THROW(w.end(), InvalidArgumentError);
    EXPECT_THROW(w.begin(Provenance()), InvalidArgumentError);
    EXPECT_THROW(w.writeFeature(r, "x", nullptr, nullptr, 0), InvalidArgumentError);
    EXPECT_THROW(GeoJsonWriter((uniqueTempPath("d", "").u8string() + "/no/such/dir/f.geojson").c_str()), IoError);
    EXPECT_THROW(GeoJsonWriter(nullptr), InvalidArgumentError);
}

TEST(GeoJsonWriter, EndBeforeBeginIsRejected) {
    auto path = uniqueTempPath("eb", ".geojson");
    TempFileGuard guard(path);
    GeoJsonWriter w(path.u8string().c_str());
    EXPECT_THROW(w.end(), InvalidArgumentError);
}

TEST(GeoJsonWriter, ArgumentValidation) {
    ObjectRecord r = triangle(2);
    auto path = uniqueTempPath("v", ".geojson");
    TempFileGuard guard(path);
    GeoJsonWriter w(path.u8string().c_str());
    w.begin(Provenance());
    const char* names[] = {"x"};
    bool local[] = {false};
    EXPECT_THROW(w.writeFeature(r, nullptr, nullptr, nullptr, 0), InvalidArgumentError);
    EXPECT_THROW(w.writeFeature(r, "l", nullptr, nullptr, 1), InvalidArgumentError);
    EXPECT_THROW(w.writeFeature(r, "l", names, nullptr, 1), InvalidArgumentError);
    EXPECT_THROW(w.writeFeature(r, "l", nullptr, local, 1), InvalidArgumentError);
    EXPECT_THROW(w.writeFeature(r, "l", names, local, -1), InvalidArgumentError);
    const char* nullName[] = {nullptr};
    EXPECT_THROW(w.writeFeature(r, "l", nullName, local, 1), InvalidArgumentError);
    r.measurements().set("bad", std::numeric_limits<double>::infinity(), MeasurementUnit::Pixels);
    EXPECT_THROW(w.writeFeature(r, "l", nullptr, nullptr, 0), InvalidArgumentError);
    r.measurements().set("bad", std::nan(""), MeasurementUnit::Pixels);
    EXPECT_THROW(w.writeFeature(r, "l", nullptr, nullptr, 0), InvalidArgumentError);
    EXPECT_EQ(w.featuresWritten(), 0);
    w.end();
    // Rejected features leave no trace in the document.
    EXPECT_NE(readFile(path).find(",\"features\":[\n\n]}\n"), std::string::npos);
}

TEST(GeoJsonWriter, DestructorDoesNotCompleteTheDocument) {
    auto path = uniqueTempPath("inc", ".geojson");
    TempFileGuard guard(path);
    {
        GeoJsonWriter w(path.u8string().c_str());
        w.begin(Provenance());
    }
    const std::string s = readFile(path);
    EXPECT_FALSE(s.empty());
    EXPECT_EQ(s.find("]}"), std::string::npos) << s;
}

TEST(GeoJsonWriter, MoveKeepsTheStream) {
    auto path = uniqueTempPath("mv", ".geojson");
    TempFileGuard guard(path);
    GeoJsonWriter a(path.u8string().c_str());
    a.begin(Provenance());
    GeoJsonWriter b(std::move(a));
    b.end();
    EXPECT_NE(readFile(path).find("]}\n"), std::string::npos);
}
