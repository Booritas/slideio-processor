// Requirements: UR-IMG-2, UR-SEG-4
#include "storage/detail/geometry_codec.hpp"

#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/geometry.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

using namespace slideio::processor;

namespace {

constexpr int64_t kMin = std::numeric_limits<int64_t>::min();
constexpr int64_t kMax = std::numeric_limits<int64_t>::max();

void addRing(Polygon& p, std::vector<Point64> points, bool isHole) {
    p.addRing(points.data(), static_cast<int64_t>(points.size()), isHole);
}

Polygon roundTrip(const Polygon& p) {
    const std::vector<uint8_t> blob = detail::encodePolygon(p);
    Polygon out;
    detail::decodePolygon(blob.data(), blob.size(), out);
    return out;
}

void expectSamePolygon(const Polygon& a, const Polygon& b) {
    ASSERT_EQ(a.ringCount(), b.ringCount());
    for (int32_t r = 0; r < a.ringCount(); ++r) {
        EXPECT_EQ(a.ringIsHole(r), b.ringIsHole(r)) << "ring " << r;
        ASSERT_EQ(a.ringPointCount(r), b.ringPointCount(r)) << "ring " << r;
        std::vector<Point64> pa(static_cast<size_t>(a.ringPointCount(r)));
        std::vector<Point64> pb(pa.size());
        a.ringPoints(r, pa.data(), static_cast<int64_t>(pa.size()));
        b.ringPoints(r, pb.data(), static_cast<int64_t>(pb.size()));
        for (size_t i = 0; i < pa.size(); ++i) {
            EXPECT_TRUE(pa[i] == pb[i]) << "ring " << r << " point " << i;
        }
    }
}

void expectCorrupt(const std::vector<uint8_t>& blob, const char* why) {
    Polygon out;
    try {
        detail::decodePolygon(blob.data(), blob.size(), out, "object (layer 2, anchor 7)");
        ADD_FAILURE() << why << ": no exception";
    } catch (const InternalError& e) {
        EXPECT_NE(std::string(e.what()).find("object (layer 2, anchor 7)"), std::string::npos)
            << why << ": " << e.what();
    } catch (const std::exception& e) {
        ADD_FAILURE() << why << ": wrong exception: " << e.what();
    }
    EXPECT_TRUE(out.isEmpty()) << why << ": output changed";
}

} // namespace

TEST(GeometryCodec, RoundTripsRingsHolesAndExtremes) {
    Polygon p;
    addRing(p, {{0, 0}, {100, 0}, {100, 100}, {0, 100}}, false);
    addRing(p, {{10, 10}, {10, 90}, {90, 90}, {90, 10}}, true);
    addRing(p, {{200, 200}, {300, 210}, {250, 400}}, false);
    expectSamePolygon(p, roundTrip(p));

    // Extents up to INT64_MAX, on both sides of 0: the largest a Polygon admits.
    Polygon negative;
    addRing(negative, {{kMin, -1}, {-1, kMin}, {-1, -1}}, false);
    expectSamePolygon(negative, roundTrip(negative));

    Polygon positive;
    addRing(positive, {{0, kMax}, {kMax, 0}, {kMax, kMax}}, false);
    expectSamePolygon(positive, roundTrip(positive));

    Polygon straddling;
    addRing(straddling, {{kMin / 2, kMax / 2}, {kMax / 2, kMin / 2}, {0, 0}, {kMin / 2, 0}}, false);
    expectSamePolygon(straddling, roundTrip(straddling));

    expectSamePolygon(Polygon(), roundTrip(Polygon()));
}

TEST(GeometryCodec, SmallDeltasAreCompact) {
    // A 25 x 25 square traced in unit steps: 100 points.
    std::vector<Point64> contour;
    const int64_t x0 = 90000;
    const int64_t y0 = 70000;
    for (int64_t i = 0; i < 25; ++i) contour.push_back({x0 + i, y0});
    for (int64_t i = 0; i < 25; ++i) contour.push_back({x0 + 25, y0 + i});
    for (int64_t i = 0; i < 25; ++i) contour.push_back({x0 + 25 - i, y0 + 25});
    for (int64_t i = 0; i < 25; ++i) contour.push_back({x0, y0 + 25 - i});
    ASSERT_EQ(contour.size(), 100u);
    Polygon p;
    addRing(p, contour, false);
    const std::vector<uint8_t> blob = detail::encodePolygon(p);
    EXPECT_LT(blob.size(), 3u * 100u);
    expectSamePolygon(p, roundTrip(p));
}

TEST(GeometryCodec, MalformedBlobsThrowInternalError) {
    Polygon square;
    addRing(square, {{0, 0}, {1000, 0}, {1000, 1000}, {0, 1000}}, false);
    const std::vector<uint8_t> valid = detail::encodePolygon(square);

    expectCorrupt({}, "empty");
    expectCorrupt({2, 0}, "version 2");
    // One ring of three points, then a varint cut off after its first byte.
    expectCorrupt({1, 1, 6, 0x80}, "truncated mid-varint");
    std::vector<uint8_t> truncated(valid.begin(), valid.end() - 1);
    expectCorrupt(truncated, "truncated last point");
    // A ring count of eleven bytes: ten with the continuation bit, then one more.
    expectCorrupt({1, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01}, "11-byte varint");
    // Ten bytes whose last carries more than the 64th bit.
    expectCorrupt({1, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x02}, "varint overflow");
    std::vector<uint8_t> trailing = valid;
    trailing.push_back(0);
    expectCorrupt(trailing, "trailing byte");
    // A ring of two points: (0,0), (2,2).
    expectCorrupt({1, 1, 4, 0, 0, 4, 4}, "ring of 2 points");
    // A ring count far beyond what the bytes could hold.
    expectCorrupt({1, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F}, "ring count beyond the blob");
    // Rings Polygon::addRing refuses (Ruling 1): collinear, and a hole first.
    expectCorrupt({1, 1, 6, 0, 0, 2, 2, 2, 2}, "collinear ring");
    expectCorrupt({1, 1, 7, 0, 0, 2, 0, 0, 2}, "hole as ring 0");
}
