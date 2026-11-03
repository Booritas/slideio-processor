#include "storage/detail/geometry_codec.hpp"

#include <slideio/processor/core/error.hpp>

#include <string>
#include <utility>

namespace slideio::processor::detail {

namespace {

constexpr uint8_t kEncodingVersion = 1;
constexpr int kMaxVarintBytes = 10;

uint64_t zigzag(int64_t v) noexcept {
    const uint64_t u = static_cast<uint64_t>(v);
    return (u << 1) ^ (v < 0 ? ~uint64_t{0} : uint64_t{0});
}

int64_t unzigzag(uint64_t u) noexcept {
    return static_cast<int64_t>((u >> 1) ^ (~(u & 1) + 1));
}

void putVarint(std::vector<uint8_t>& out, uint64_t v) {
    while (v >= 0x80) {
        out.push_back(static_cast<uint8_t>(v | 0x80));
        v >>= 7;
    }
    out.push_back(static_cast<uint8_t>(v));
}

class Reader {
public:
    Reader(const uint8_t* data, size_t size, const char* objectName)
        : data_(data), size_(size), name_(objectName != nullptr ? objectName : "geometry blob") {}

    [[noreturn]] void corrupt(const char* reason) const {
        const std::string message =
            std::string("store geometry of ") + name_ + " is corrupt: " + reason;
        throw InternalError(message.c_str());
    }

    size_t remaining() const noexcept { return size_ - pos_; }

    uint8_t byte() {
        if (pos_ >= size_) corrupt("truncated");
        return data_[pos_++];
    }

    uint64_t varint() {
        uint64_t v = 0;
        for (int i = 0; i < kMaxVarintBytes; ++i) {
            const uint8_t b = byte();
            if (i == kMaxVarintBytes - 1 && b > 1) corrupt("over-long varint");
            v |= static_cast<uint64_t>(b & 0x7F) << (7 * i);
            if ((b & 0x80) == 0) return v;
        }
        corrupt("over-long varint");
    }

private:
    const uint8_t* data_;
    size_t size_;
    size_t pos_ = 0;
    const char* name_;
};

} // namespace

std::vector<uint8_t> encodePolygon(const Polygon& polygon) {
    std::vector<uint8_t> out;
    const int32_t rings = polygon.ringCount();
    out.push_back(kEncodingVersion);
    putVarint(out, static_cast<uint64_t>(rings));
    std::vector<Point64> points;
    for (int32_t r = 0; r < rings; ++r) {
        const int64_t count = polygon.ringPointCount(r);
        points.resize(static_cast<size_t>(count));
        polygon.ringPoints(r, points.data(), count);
        putVarint(out, (static_cast<uint64_t>(count) << 1) | (polygon.ringIsHole(r) ? 1u : 0u));
        uint64_t prevX = 0;
        uint64_t prevY = 0;
        for (const Point64& p : points) {
            const uint64_t x = static_cast<uint64_t>(p.x);
            const uint64_t y = static_cast<uint64_t>(p.y);
            // The first point's "previous" is (0, 0), so its deltas are its absolute coordinates.
            putVarint(out, zigzag(static_cast<int64_t>(x - prevX)));
            putVarint(out, zigzag(static_cast<int64_t>(y - prevY)));
            prevX = x;
            prevY = y;
        }
    }
    return out;
}

void decodePolygon(const uint8_t* data, size_t size, Polygon& out, const char* objectName) {
    Reader in(data, size, objectName);
    if (data == nullptr || size == 0) in.corrupt("empty");
    if (in.byte() != kEncodingVersion) in.corrupt("unknown encoding version");
    const uint64_t rings = in.varint();
    // Every ring takes at least one header byte.
    if (rings > in.remaining()) in.corrupt("ring count exceeds the blob");
    Polygon polygon;
    std::vector<Point64> points;
    for (uint64_t r = 0; r < rings; ++r) {
        const uint64_t header = in.varint();
        const uint64_t count = header >> 1;
        const bool isHole = (header & 1) != 0;
        if (count < 3) in.corrupt("ring of fewer than 3 points");
        if (count > in.remaining() / 2) in.corrupt("truncated");
        points.resize(static_cast<size_t>(count));
        uint64_t x = 0;
        uint64_t y = 0;
        for (Point64& p : points) {
            x += static_cast<uint64_t>(unzigzag(in.varint()));
            y += static_cast<uint64_t>(unzigzag(in.varint()));
            p = {static_cast<int64_t>(x), static_cast<int64_t>(y)};
        }
        try {
            polygon.addRing(points.data(), static_cast<int64_t>(count), isHole);
        } catch (const InvalidArgumentError& e) {
            in.corrupt(e.what());
        }
    }
    if (in.remaining() != 0) in.corrupt("trailing bytes");
    out = std::move(polygon);
}

} // namespace slideio::processor::detail
