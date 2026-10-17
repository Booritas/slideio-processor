#include <slideio/processor/core/ids.hpp>

#include <charconv>
#include <cstring>

namespace slideio::processor {

LayerId::LayerId() noexcept : value_(0) {}
LayerId::LayerId(int32_t value) noexcept : value_(value) {}
int32_t LayerId::value() const noexcept { return value_; }
bool LayerId::isValid() const noexcept { return value_ > 0; }
bool LayerId::operator==(LayerId other) const noexcept { return value_ == other.value_; }
bool LayerId::operator!=(LayerId other) const noexcept { return value_ != other.value_; }
bool LayerId::operator<(LayerId other) const noexcept { return value_ < other.value_; }

StageId::StageId() noexcept : value_(0) {}
StageId::StageId(int32_t value) noexcept : value_(value) {}
int32_t StageId::value() const noexcept { return value_; }
bool StageId::isValid() const noexcept { return value_ > 0; }
bool StageId::operator==(StageId other) const noexcept { return value_ == other.value_; }
bool StageId::operator!=(StageId other) const noexcept { return value_ != other.value_; }
bool StageId::operator<(StageId other) const noexcept { return value_ < other.value_; }

LabelId::LabelId() noexcept : value_(0) {}
LabelId::LabelId(int32_t value) noexcept : value_(value) {}
int32_t LabelId::value() const noexcept { return value_; }
bool LabelId::isValid() const noexcept { return value_ > 0; }
bool LabelId::operator==(LabelId other) const noexcept { return value_ == other.value_; }
bool LabelId::operator!=(LabelId other) const noexcept { return value_ != other.value_; }
bool LabelId::operator<(LabelId other) const noexcept { return value_ < other.value_; }

RasterId::RasterId() noexcept : value_(0) {}
RasterId::RasterId(int32_t value) noexcept : value_(value) {}
int32_t RasterId::value() const noexcept { return value_; }
bool RasterId::isValid() const noexcept { return value_ > 0; }
bool RasterId::operator==(RasterId other) const noexcept { return value_ == other.value_; }
bool RasterId::operator!=(RasterId other) const noexcept { return value_ != other.value_; }
bool RasterId::operator<(RasterId other) const noexcept { return value_ < other.value_; }

namespace {

constexpr int64_t kMaxAnchorX = (int64_t(1) << 31) - 1;
constexpr int64_t kMaxAnchorY = (int64_t(1) << 32) - 1;

// Parses unsigned decimal digits at p. With terminator ':' the digits must be
// followed by it (consumed); with '\0' they must run to `end`.
bool parseField(const char*& p, const char* end, char terminator, int64_t& value) {
    if (p == end || *p < '0' || *p > '9') {
        return false;
    }
    if (*p == '0' && p + 1 != end && p[1] >= '0' && p[1] <= '9') {
        return false; // leading zero
    }
    const auto r = std::from_chars(p, end, value);
    if (r.ec != std::errc()) {
        return false;
    }
    p = r.ptr;
    if (terminator == '\0') {
        return p == end;
    }
    if (p == end || *p != terminator) {
        return false;
    }
    ++p;
    return true;
}

} // namespace

ObjectId::ObjectId() noexcept : layer_(), packed_(-1) {}

ObjectId ObjectId::fromAnchor(LayerId layer, Point64 anchor) noexcept {
    ObjectId id;
    if (layer.isValid() && anchor.x >= 0 && anchor.x <= kMaxAnchorX && anchor.y >= 0 &&
        anchor.y <= kMaxAnchorY) {
        id.layer_ = layer;
        id.packed_ = (anchor.y << 31) | anchor.x;
    }
    return id;
}

bool ObjectId::fromString(const char* text, ObjectId& out) noexcept {
    if (text == nullptr) {
        return false;
    }
    const char* p = text;
    const char* end = text + std::strlen(text);
    int64_t layer = 0, x = 0, y = 0;
    if (!parseField(p, end, ':', layer) || !parseField(p, end, ':', x) ||
        !parseField(p, end, '\0', y)) {
        return false;
    }
    if (layer < 1 || layer > INT32_MAX) {
        return false;
    }
    const ObjectId id = fromAnchor(LayerId(static_cast<int32_t>(layer)), Point64{x, y});
    if (!id.isValid()) {
        return false;
    }
    out = id;
    return true;
}

bool ObjectId::isValid() const noexcept { return packed_ >= 0; }

LayerId ObjectId::layer() const noexcept { return layer_; }

int64_t ObjectId::anchorPacked() const noexcept { return packed_; }

Point64 ObjectId::anchorPoint() const noexcept {
    if (!isValid()) {
        return Point64{0, 0};
    }
    return Point64{packed_ & kMaxAnchorX, packed_ >> 31};
}

int32_t ObjectId::toString(char* buffer, int32_t capacity) const {
    char text[64];
    int32_t length = 0;
    if (!isValid()) {
        std::memcpy(text, "invalid", 8);
        length = 7;
    } else {
        char* p = text;
        const Point64 a = anchorPoint();
        const int64_t fields[3] = {layer_.value(), a.x, a.y};
        for (int i = 0; i < 3; ++i) {
            if (i > 0) {
                *p++ = ':';
            }
            p = std::to_chars(p, text + sizeof(text) - 1, fields[i]).ptr;
        }
        *p = '\0';
        length = static_cast<int32_t>(p - text);
    }
    if (buffer != nullptr && capacity > 0) {
        const int32_t n = length < capacity - 1 ? length : capacity - 1;
        std::memcpy(buffer, text, static_cast<size_t>(n));
        buffer[n] = '\0';
    }
    return length;
}

bool ObjectId::operator==(const ObjectId& other) const noexcept {
    return layer_ == other.layer_ && packed_ == other.packed_;
}

bool ObjectId::operator!=(const ObjectId& other) const noexcept { return !(*this == other); }

bool ObjectId::operator<(const ObjectId& other) const noexcept {
    if (layer_ != other.layer_) {
        return layer_ < other.layer_;
    }
    return packed_ < other.packed_;
}

} // namespace slideio::processor
