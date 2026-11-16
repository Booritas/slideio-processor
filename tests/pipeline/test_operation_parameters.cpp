// Requirements: UR-STG-4, UR-RES-11
#include <slideio/processor/core/error.hpp>
#include <slideio/processor/pipeline/operation_parameters.hpp>

#include <pipeline/detail/parameters_json.hpp>

#include <gtest/gtest.h>

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

using slideio::processor::InvalidArgumentError;
using slideio::processor::OperationParameters;

namespace {
std::string jsonOf(const OperationParameters& p) {
    const int32_t n = p.toJson(nullptr, 0);
    std::vector<char> buf(static_cast<size_t>(n) + 1);
    p.toJson(buf.data(), n + 1);
    return std::string(buf.data(), static_cast<size_t>(n));
}
} // namespace

TEST(OperationParameters, TypedGettersNeverCoerce) {
    OperationParameters p;
    p.setInt("n", 3).setDouble("s", 1.5);
    int64_t i = 0;
    double d = 0;
    bool b = false;
    EXPECT_TRUE(p.getInt("n", i));
    EXPECT_EQ(i, 3);
    EXPECT_TRUE(p.getDouble("s", d));
    EXPECT_EQ(d, 1.5);
    EXPECT_FALSE(p.getInt("s", i));
    EXPECT_FALSE(p.getDouble("n", d));
    EXPECT_FALSE(p.getBool("n", b));
    EXPECT_FALSE(p.getInt("absent", i));
    EXPECT_STREQ(p.getString("n", "fb"), "fb");
    EXPECT_STREQ(p.getString("absent", "fb"), "fb");
    EXPECT_EQ(p.getDoubleArray("n", nullptr, 0), 0);
}

TEST(OperationParameters, KeysAreOrderedAndReplaceInPlace) {
    OperationParameters p;
    p.setInt("b", 1).setInt("a", 2).setInt("c", 3);
    p.setString("a", "x");
    ASSERT_EQ(p.count(), 3);
    EXPECT_STREQ(p.keyAt(0), "a");
    EXPECT_STREQ(p.keyAt(1), "b");
    EXPECT_STREQ(p.keyAt(2), "c");
    EXPECT_STREQ(p.getString("a", ""), "x");
    int64_t i = 0;
    EXPECT_FALSE(p.getInt("a", i));
    EXPECT_THROW(p.keyAt(3), InvalidArgumentError);
    EXPECT_THROW(p.keyAt(-1), InvalidArgumentError);

    OperationParameters q;
    q.setInt("c", 3).setInt("b", 1).setString("a", "x");
    EXPECT_TRUE(slideio::processor::detail::parametersEqual(p, q));
    EXPECT_EQ(jsonOf(p), jsonOf(q));
}

TEST(OperationParameters, RejectsNonFiniteAndBadKeys) {
    OperationParameters p;
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    EXPECT_THROW(p.setDouble("x", nan), InvalidArgumentError);
    EXPECT_THROW(p.setDouble("x", inf), InvalidArgumentError);
    const double bad[] = {1, nan};
    EXPECT_THROW(p.setDoubleArray("x", bad, 2), InvalidArgumentError);
    EXPECT_THROW(p.setInt(nullptr, 1), InvalidArgumentError);
    EXPECT_THROW(p.setInt("", 1), InvalidArgumentError);
    EXPECT_THROW(p.setBool(nullptr, true), InvalidArgumentError);
    EXPECT_THROW(p.setString("", "v"), InvalidArgumentError);
    EXPECT_THROW(p.setDoubleArray("x", nullptr, 2), InvalidArgumentError);
    EXPECT_THROW(p.setDoubleArray("x", bad, -1), InvalidArgumentError);
    EXPECT_EQ(p.count(), 0);
    try {
        p.setDouble("myKey", nan);
        FAIL();
    } catch (const InvalidArgumentError& e) {
        EXPECT_NE(std::string(e.what()).find("myKey"), std::string::npos);
    }
}

TEST(OperationParameters, DoubleArrayFollowsC5) {
    OperationParameters p;
    const double v[] = {1, 2, 3};
    p.setDoubleArray("k", v, 3);
    EXPECT_EQ(p.getDoubleArray("k", nullptr, 0), 3);
    double out[3] = {-1, -1, -1};
    EXPECT_EQ(p.getDoubleArray("k", out, 2), 3);
    EXPECT_EQ(out[0], 1);
    EXPECT_EQ(out[1], 2);
    EXPECT_EQ(out[2], -1);
    p.setDoubleArray("e", nullptr, 0);
    EXPECT_EQ(p.count(), 2);
    EXPECT_EQ(p.getDoubleArray("e", nullptr, 0), 0);
}

TEST(OperationParameters, CopyMoveAssign) {
    OperationParameters p;
    p.setInt("a", 1).setString("s", "v");
    OperationParameters c(p);
    OperationParameters a;
    a = p;
    OperationParameters m(std::move(c));
    EXPECT_TRUE(slideio::processor::detail::parametersEqual(p, a));
    EXPECT_TRUE(slideio::processor::detail::parametersEqual(p, m));
    OperationParameters n;
    n = std::move(a);
    EXPECT_TRUE(slideio::processor::detail::parametersEqual(p, n));
    // A moved-from bag is empty and reusable.
    EXPECT_EQ(c.count(), 0);
    c.setInt("z", 9);
    EXPECT_EQ(c.count(), 1);
    // A modification of the copy leaves the original alone.
    n.setInt("a", 2);
    int64_t i = 0;
    ASSERT_TRUE(p.getInt("a", i));
    EXPECT_EQ(i, 1);
}

TEST(OperationParameters, JsonRoundTripPreservesTypes) {
    OperationParameters p;
    p.setInt("i0", 0).setInt("im1", -1)
        .setInt("imax", std::numeric_limits<int64_t>::max())
        .setInt("imin", std::numeric_limits<int64_t>::min())
        .setDouble("tenth", 0.1).setDouble("one", 1.0).setDouble("negzero", -0.0)
        .setDouble("big", 1e300)
        .setBool("t", true).setBool("f", false)
        .setString("unit", "\xC2\xB5m \xE2\x9C\x93")
        .setDoubleArray("empty", nullptr, 0)
        .setInt("\xD0\xBA\xD0\xBB\xD1\x8E\xD1\x87", 7);
    const double arr[] = {1.0, 0.5, -2};
    p.setDoubleArray("arr", arr, 3);

    const std::string text = jsonOf(p);
    rapidjson::Document doc;
    doc.Parse<rapidjson::kParseFullPrecisionFlag>(text.c_str());
    ASSERT_FALSE(doc.HasParseError());
    const OperationParameters back = slideio::processor::detail::parametersFromJson(doc, "test");
    EXPECT_TRUE(slideio::processor::detail::parametersEqual(p, back));
    double d = 0;
    int64_t i = 0;
    EXPECT_TRUE(back.getDouble("one", d));
    EXPECT_FALSE(back.getInt("one", i));
    EXPECT_TRUE(back.getInt("imax", i));
    EXPECT_EQ(i, std::numeric_limits<int64_t>::max());
    EXPECT_TRUE(back.getInt("imin", i));
    EXPECT_EQ(i, std::numeric_limits<int64_t>::min());
    EXPECT_TRUE(back.getDouble("negzero", d));
    EXPECT_TRUE(std::signbit(d));
    EXPECT_TRUE(back.getDouble("tenth", d));
    EXPECT_EQ(d, 0.1);
    // A re-serialised bag is byte-identical.
    EXPECT_EQ(jsonOf(back), text);
}

TEST(OperationParameters, FromJsonRejectsBadInput) {
    using slideio::processor::detail::parametersFromJson;
    rapidjson::Document notObject;
    notObject.Parse("[1]");
    EXPECT_THROW(parametersFromJson(notObject, "stage 3"), InvalidArgumentError);
    rapidjson::Document nested;
    nested.Parse("{\"k\":{\"a\":1}}");
    try {
        parametersFromJson(nested, "stage 3");
        FAIL();
    } catch (const InvalidArgumentError& e) {
        const std::string m = e.what();
        EXPECT_NE(m.find("stage 3"), std::string::npos);
        EXPECT_NE(m.find("k"), std::string::npos);
    }
    rapidjson::Document mixed;
    mixed.Parse("{\"k\":[1,\"x\"]}");
    EXPECT_THROW(parametersFromJson(mixed, "w"), InvalidArgumentError);
    rapidjson::Document nul;
    nul.Parse("{\"k\":null}");
    EXPECT_THROW(parametersFromJson(nul, "w"), InvalidArgumentError);
    rapidjson::Document emptyKey;
    emptyKey.Parse("{\"\":1}");
    EXPECT_THROW(parametersFromJson(emptyKey, "w"), InvalidArgumentError);
    // An integer outside the int64 range has no int form and is refused, not rounded.
    rapidjson::Document huge;
    huge.Parse("{\"k\":18446744073709551615}");
    EXPECT_THROW(parametersFromJson(huge, "w"), InvalidArgumentError);
}

TEST(OperationParameters, ToJsonFollowsC4) {
    OperationParameters p;
    p.setInt("a", 1).setString("s", "v");
    const int32_t n = p.toJson(nullptr, 0);
    ASSERT_GT(n, 5);
    std::string full(static_cast<size_t>(n) + 1, 'x');
    EXPECT_EQ(p.toJson(&full[0], n + 1), n);
    EXPECT_EQ(full[static_cast<size_t>(n)], '\0');
    EXPECT_EQ(full.substr(0, static_cast<size_t>(n)), "{\"a\":1,\"s\":\"v\"}");
    char small[5];
    std::fill(small, small + 5, 'x');
    EXPECT_EQ(p.toJson(small, 5), n);
    EXPECT_EQ(std::string(small, 4), full.substr(0, 4));
    EXPECT_EQ(small[4], '\0');
    EXPECT_EQ(OperationParameters().toJson(nullptr, 0), 2);
}
