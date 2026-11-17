// Requirements: UR-EXE-2, UR-SEG-3
#include <pipeline/detail/fragment_spill.hpp>

#include <slideio/processor/core/error.hpp>

#include <cstdio>
#include <cstring>
#include <random>
#include <system_error>

namespace slideio::processor::detail {

namespace {

constexpr size_t kHexDigits = 32;

template <typename T>
void put(std::vector<char>& buffer, const T& value) {
    const size_t at = buffer.size();
    buffer.resize(at + sizeof(T));
    std::memcpy(buffer.data() + at, &value, sizeof(T));
}

} // namespace

std::string randomHexName() {
    std::random_device device;
    std::string name;
    for (size_t i = 0; i < kHexDigits / 8; ++i) {
        char word[9];
        std::snprintf(word, sizeof(word), "%08x", static_cast<unsigned>(device()));
        name += word;
    }
    return name;
}

FragmentSpill::FragmentSpill(const std::string& directory)
    : path_(std::filesystem::u8path(directory) / ("seam-" + randomHexName() + ".bin")),
      pathText_(path_.u8string()) {
    file_.open(path_, std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
    if (!file_.is_open()) {
        throw IoError(("SeamMerger: cannot create the scratch file '" + pathText_ + "'").c_str());
    }
}

FragmentSpill::~FragmentSpill() {
    if (file_.is_open()) {
        file_.close();
    }
    std::error_code error;
    std::filesystem::remove(path_, error);
}

void FragmentSpill::fail(const char* what) {
    file_.clear();
    throw IoError(("SeamMerger: cannot " + std::string(what) + " the scratch file '" + pathText_ + "'").c_str());
}

int64_t FragmentSpill::write(const Polygon& polygon, const std::vector<IntensityPartial>& partials, int64_t& bytes) {
    std::vector<char> buffer;
    const int32_t ringCount = polygon.ringCount();
    put(buffer, ringCount);
    std::vector<Point64> points;
    for (int32_t ring = 0; ring < ringCount; ++ring) {
        const int64_t pointCount = polygon.ringPointCount(ring);
        points.resize(static_cast<size_t>(pointCount));
        polygon.ringPoints(ring, points.data(), pointCount);
        put(buffer, static_cast<uint8_t>(polygon.ringIsHole(ring) ? 1 : 0));
        put(buffer, pointCount);
        for (const Point64& point : points) {
            put(buffer, point.x);
            put(buffer, point.y);
        }
    }
    put(buffer, static_cast<int32_t>(partials.size()));
    for (const IntensityPartial& partial : partials) {
        put(buffer, partial.count);
        put(buffer, partial.sum);
        put(buffer, partial.sumOfSquares);
        put(buffer, partial.minimum);
        put(buffer, partial.maximum);
        put(buffer, partial.channel);
    }

    std::lock_guard<std::mutex> lock(mutex_);
    const int64_t offset = size_;
    file_.seekp(static_cast<std::streamoff>(offset));
    file_.write(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    file_.flush();
    if (!file_) {
        fail("write");
    }
    size_ += static_cast<int64_t>(buffer.size());
    bytes = static_cast<int64_t>(buffer.size());
    return offset;
}

void FragmentSpill::read(int64_t offset, Polygon& polygon, std::vector<IntensityPartial>& partials) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto get = [this](auto& value) {
        file_.read(reinterpret_cast<char*>(&value), sizeof(value));
        if (!file_) {
            fail("read");
        }
    };
    if (offset < 0 || offset >= size_) {
        fail("read");
    }
    file_.seekg(static_cast<std::streamoff>(offset));
    if (!file_) {
        fail("read");
    }
    polygon.clear();
    int32_t ringCount = 0;
    get(ringCount);
    if (ringCount < 0) {
        fail("read");
    }
    std::vector<Point64> points;
    for (int32_t ring = 0; ring < ringCount; ++ring) {
        uint8_t hole = 0;
        int64_t pointCount = 0;
        get(hole);
        get(pointCount);
        if (pointCount < 0 || pointCount > (size_ - offset) / 16) {
            fail("read");
        }
        points.resize(static_cast<size_t>(pointCount));
        for (Point64& point : points) {
            get(point.x);
            get(point.y);
        }
        polygon.addRing(points.data(), pointCount, hole != 0);
    }
    int32_t partialCount = 0;
    get(partialCount);
    if (partialCount < 0 || partialCount > (size_ - offset) / 44) {
        fail("read");
    }
    partials.resize(static_cast<size_t>(partialCount));
    for (IntensityPartial& partial : partials) {
        get(partial.count);
        get(partial.sum);
        get(partial.sumOfSquares);
        get(partial.minimum);
        get(partial.maximum);
        get(partial.channel);
    }
}

} // namespace slideio::processor::detail
