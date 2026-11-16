// Requirements: UR-STG-4, UR-RES-11, UR-EXE-1, UR-IMG-5
#include <slideio/processor/core/error.hpp>
#include <slideio/processor/pipeline/operation_catalog.hpp>
#include <slideio/processor/pipeline/pipeline_spec.hpp>

#include <pipeline/test_support/test_operations.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <utility>
#include <string>
#include <vector>

using namespace slideio::processor;

namespace {

std::string jsonOf(const PipelineSpec& s) {
    const int32_t n = s.toJson(nullptr, 0);
    std::vector<char> buf(static_cast<size_t>(n) + 1);
    s.toJson(buf.data(), n + 1);
    return std::string(buf.data(), static_cast<size_t>(n));
}

Polygon ringsPolygon(bool holeFlag = true) {
    const Point64 outer[] = {{0, 0}, {100, 0}, {100, 80}, {0, 80}};
    const Point64 hole[] = {{20, 20}, {20, 40}, {40, 40}, {40, 20}};
    Polygon p;
    p.addRing(outer, 4, false);
    p.addRing(hole, 4, holeFlag);
    return p;
}

PipelineSpec fullSpec() {
    ChannelSelection ch;
    ch.add(2).add(0);
    OperationParameters a;
    a.setInt("big", std::numeric_limits<int64_t>::max() - 1)
        .setInt("neg", -7)
        .setDouble("tenth", 0.1)
        .setDouble("whole", 3.0)
        .setBool("flag", true)
        .setString("text", "caf\xC3\xA9 \"q\"")
        .setDoubleArray("arr", std::vector<double>{1.5, -2.25, 1e-9}.data(), 3)
        .setDoubleArray("empty", nullptr, 0)
        .setInt("k\xC3\xA9y", 1);
    OperationParameters b;
    b.setDouble("level", 99.5);
    PipelineSpec s;
    s.channels(ch).analysisLevel(3).restrictTo(Rect64{10, 20, 300, 400}).restrictToPolygon(ringsPolygon());
    s.operation("test.threshold", a).operation("test.boxMean", b);
    return s;
}

bool throwsInvalid(const char* json, std::string* message = nullptr) {
    try {
        PipelineSpec::fromJson(json);
    } catch (const InvalidArgumentError& e) {
        if (message != nullptr) {
            *message = e.what();
        }
        return true;
    }
    return false;
}

} // namespace

TEST(PipelineSpec, NamedAndConstructedOperationsBuildTheSameSpec) {
    testing_support::registerTestOperations();
    OperationParameters params;
    params.setDouble("level", 42.0);
    PipelineSpec a;
    a.operation("test.threshold", params);
    PipelineSpec b;
    b.then(OperationCatalog::instance().create("test.threshold", params));
    EXPECT_TRUE(a.equals(b));
    EXPECT_STREQ(b.operationName(0), "test.threshold");
}

TEST(PipelineSpec, RestrictionsReplaceRatherThanAccumulate) {
    PipelineSpec s;
    EXPECT_FALSE(s.hasRoi());
    EXPECT_TRUE(s.roi().isEmpty());
    EXPECT_FALSE(s.hasPolygon());
    EXPECT_TRUE(s.polygon().isEmpty());
    s.restrictTo(Rect64{1, 2, 3, 4}).restrictTo(Rect64{5, 6, 7, 8});
    EXPECT_TRUE(s.hasRoi());
    EXPECT_EQ(s.roi().x, 5);
    EXPECT_EQ(s.roi().height, 8);
    const Point64 tri[] = {{0, 0}, {10, 0}, {0, 10}};
    Polygon one;
    one.addRing(tri, 3, false);
    s.restrictToPolygon(ringsPolygon()).restrictToPolygon(one);
    EXPECT_TRUE(s.hasPolygon());
    EXPECT_EQ(s.polygon().ringCount(), 1);
}

TEST(PipelineSpec, JsonRoundTripIsExact) {
    const PipelineSpec original = fullSpec();
    const std::string json = jsonOf(original);
    const PipelineSpec copy = PipelineSpec::fromJson(json.c_str());
    EXPECT_TRUE(copy.equals(original));
    EXPECT_EQ(jsonOf(copy), json);
    EXPECT_EQ(copy.channels().count(), 2);
    EXPECT_EQ(copy.channels().at(0), 2);
    double tenth = 0;
    ASSERT_TRUE(copy.operationParameters(0).getDouble("tenth", tenth));
    EXPECT_EQ(tenth, 0.1);

    // A spec with nothing optional set.
    PipelineSpec plain;
    plain.operation("test.threshold", OperationParameters());
    const PipelineSpec plainCopy = PipelineSpec::fromJson(jsonOf(plain).c_str());
    EXPECT_TRUE(plainCopy.equals(plain));
    EXPECT_TRUE(plainCopy.channels().isAll());
    EXPECT_EQ(plainCopy.analysisLevel(), -1);
    EXPECT_FALSE(plainCopy.hasRoi());
    EXPECT_FALSE(plainCopy.hasPolygon());
}

TEST(PipelineSpec, EqualsIgnoresParameterSetOrder) {
    OperationParameters p1;
    p1.setInt("a", 1).setString("b", "x").setDouble("c", 2.5);
    OperationParameters p2;
    p2.setDouble("c", 2.5).setString("b", "x").setInt("a", 1);
    PipelineSpec s1;
    s1.operation("n", p1);
    PipelineSpec s2;
    s2.operation("n", p2);
    EXPECT_TRUE(s1.equals(s2));
    EXPECT_EQ(jsonOf(s1), jsonOf(s2));
}

TEST(PipelineSpec, EqualsSeesEveryField) {
    const PipelineSpec base = fullSpec();
    EXPECT_TRUE(base.equals(fullSpec()));

    {
        PipelineSpec s = fullSpec();
        s.analysisLevel(2);
        EXPECT_FALSE(base.equals(s));
    }
    {
        PipelineSpec s = fullSpec();
        ChannelSelection ch;
        ch.add(0).add(2);  // same set, different order
        s.channels(ch);
        EXPECT_FALSE(base.equals(s));
        s.channels(ChannelSelection::all());
        EXPECT_FALSE(base.equals(s));
    }
    {
        PipelineSpec s = fullSpec();
        s.restrictTo(Rect64{10, 20, 300, 401});
        EXPECT_FALSE(base.equals(s));
    }
    {
        PipelineSpec s = fullSpec();
        s.restrictToPolygon(ringsPolygon(false));  // hole flag differs
        EXPECT_FALSE(base.equals(s));
    }
    {
        PipelineSpec s = fullSpec();
        const Point64 tri[] = {{0, 0}, {10, 0}, {0, 10}};
        Polygon p;
        p.addRing(tri, 3, false);
        s.restrictToPolygon(p);
        EXPECT_FALSE(base.equals(s));
    }
    {
        PipelineSpec s = fullSpec();
        OperationParameters b;
        b.setDouble("level", 99.75);
        PipelineSpec t;
        t.channels(base.channels()).analysisLevel(3).restrictTo(base.roi()).restrictToPolygon(base.polygon());
        t.operation(base.operationName(0), base.operationParameters(0)).operation("test.boxMean", b);
        EXPECT_FALSE(base.equals(t));
        PipelineSpec u;
        u.channels(base.channels()).analysisLevel(3).restrictTo(base.roi()).restrictToPolygon(base.polygon());
        u.operation(base.operationName(0), base.operationParameters(0));
        EXPECT_FALSE(base.equals(u));  // fewer operations
        PipelineSpec v;
        v.channels(base.channels()).analysisLevel(3).restrictTo(base.roi()).restrictToPolygon(base.polygon());
        v.operation("other.name", base.operationParameters(0)).operation(base.operationName(1), base.operationParameters(1));
        EXPECT_FALSE(base.equals(v));  // different name
    }
    {
        PipelineSpec none;
        PipelineSpec roiOnly;
        roiOnly.restrictTo(Rect64{0, 0, 1, 1});
        EXPECT_FALSE(none.equals(roiOnly));
    }
}

TEST(PipelineSpec, FromJsonNamesTheBadMember) {
    std::string m;
    ASSERT_TRUE(throwsInvalid("{", &m));
    EXPECT_NE(m.find("offset"), std::string::npos) << m;
    ASSERT_TRUE(throwsInvalid(nullptr));

    ASSERT_TRUE(throwsInvalid("{\"specVersion\":1,\"channels\":\"all\",\"analysisLevel\":-1}", &m));
    EXPECT_NE(m.find("operations"), std::string::npos) << m;
    ASSERT_TRUE(throwsInvalid(
        "{\"specVersion\":1,\"channels\":\"all\",\"analysisLevel\":\"x\",\"operations\":[]}", &m));
    EXPECT_NE(m.find("analysisLevel"), std::string::npos) << m;
    ASSERT_TRUE(throwsInvalid(
        "{\"specVersion\":2,\"channels\":\"all\",\"analysisLevel\":-1,\"operations\":[]}", &m));
    EXPECT_NE(m.find("specVersion"), std::string::npos) << m;
    ASSERT_TRUE(throwsInvalid(
        "{\"specVersion\":1,\"channels\":\"some\",\"analysisLevel\":-1,\"operations\":[]}", &m));
    EXPECT_NE(m.find("channels"), std::string::npos) << m;
    ASSERT_TRUE(throwsInvalid(
        "{\"specVersion\":1,\"channels\":\"all\",\"analysisLevel\":-1,\"roi\":[1,2,3],\"operations\":[]}", &m));
    EXPECT_NE(m.find("roi"), std::string::npos) << m;
    ASSERT_TRUE(throwsInvalid(
        "{\"specVersion\":1,\"channels\":\"all\",\"analysisLevel\":-1,\"polygon\":[{\"hole\":1,\"points\":[]}],"
        "\"operations\":[]}", &m));
    EXPECT_NE(m.find("polygon"), std::string::npos) << m;
    ASSERT_TRUE(throwsInvalid(
        "{\"specVersion\":1,\"channels\":\"all\",\"analysisLevel\":-1,\"operations\":[{\"parameters\":{}}]}", &m));
    EXPECT_NE(m.find("name"), std::string::npos) << m;
    ASSERT_TRUE(throwsInvalid("[]"));
}

TEST(PipelineSpec, Validation) {
    PipelineSpec s;
    EXPECT_THROW(s.restrictTo(Rect64{0, 0, 0, 5}), InvalidArgumentError);
    EXPECT_THROW(s.restrictToPolygon(Polygon()), InvalidArgumentError);
    EXPECT_THROW(s.analysisLevel(-2), InvalidArgumentError);
    EXPECT_NO_THROW(s.analysisLevel(-1));
    EXPECT_THROW(s.then(nullptr), InvalidArgumentError);
    EXPECT_THROW(s.operation("", OperationParameters()), InvalidArgumentError);
    EXPECT_THROW(s.operation(nullptr, OperationParameters()), InvalidArgumentError);
    EXPECT_NO_THROW(s.operation("not.registered", OperationParameters()));
    EXPECT_EQ(s.operationCount(), 1);
    EXPECT_THROW(s.operationName(1), InvalidArgumentError);
    EXPECT_THROW(s.operationName(-1), InvalidArgumentError);
    EXPECT_THROW(s.operationParameters(1), InvalidArgumentError);
    EXPECT_FALSE(s.hasRoi());
}

TEST(PipelineSpec, CopyAndMove) {
    PipelineSpec a = fullSpec();
    PipelineSpec b(a);
    EXPECT_TRUE(a.equals(b));
    PipelineSpec c;
    c = a;
    EXPECT_TRUE(a.equals(c));
    PipelineSpec d(std::move(b));
    EXPECT_TRUE(a.equals(d));
    PipelineSpec moved;
    moved = std::move(d);
    EXPECT_TRUE(a.equals(moved));
    EXPECT_TRUE(b.equals(PipelineSpec()));  // moved-from is the default state
}
