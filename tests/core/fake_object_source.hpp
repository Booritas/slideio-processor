// Test-only: an in-memory ObjectSource for the layer-source tests.
#pragma once

#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/flow.hpp>

#include <cstdint>
#include <utility>
#include <vector>

namespace slideio::processor {

// A square object of side `s` with its top-left corner at (x, y) on `layer`.
inline ObjectRecord rec(int32_t layer, int64_t x, int64_t y, int64_t s, int32_t label = 0) {
    ObjectRecord r;
    const Point64 ring[4] = {{x, y}, {x + s, y}, {x + s, y + s}, {x, y + s}};
    Polygon poly;
    poly.addRing(ring, 4, false);
    r.setLayer(LayerId(layer));
    r.setGeometry(poly);
    if (label > 0) {
        r.setLabel(LabelId(label));
    }
    return r;
}

class FakeCursor final : public Cursor {
public:
    explicit FakeCursor(std::vector<ObjectRecord> records, bool* closed = nullptr)
        : records_(std::move(records)), closed_(closed) {}
    bool next() override { return ++pos_ < static_cast<int64_t>(records_.size()); }
    const ObjectRecord& current() const override {
        if (pos_ < 0 || pos_ >= static_cast<int64_t>(records_.size())) {
            throw InvalidArgumentError("FakeCursor: no current object");
        }
        return records_[static_cast<size_t>(pos_)];
    }
    int64_t position() const override { return pos_; }
    void close() override {
        if (closed_) {
            *closed_ = true;
        }
    }

private:
    std::vector<ObjectRecord> records_;
    int64_t pos_ = -1;
    bool* closed_;
};

class FakeObjectSource final : public ObjectSource {
public:
    explicit FakeObjectSource(std::vector<ObjectRecord> records) : records_(std::move(records)) {}

    CursorPtr query(const ObjectQuery& q) override {
        last_ = q;
        return std::make_shared<FakeCursor>(matching(q), &lastCursorClosed_);
    }
    int64_t count(const ObjectQuery& q) const override {
        return static_cast<int64_t>(matching(q).size());
    }
    bool get(ObjectId id, ObjectRecord& out) const override {
        for (const ObjectRecord& r : records_) {
            if (r.id() == id) {
                out = r;
                return true;
            }
        }
        return false;
    }

    const ObjectQuery& lastQuery() const { return last_; }
    bool lastCursorClosed() const { return lastCursorClosed_; }

private:
    std::vector<ObjectRecord> matching(const ObjectQuery& q) const {
        std::vector<ObjectRecord> out;
        for (const ObjectRecord& r : records_) {
            if (q.hasLayer() && r.layer() != q.layerValue()) {
                continue;
            }
            if (q.hasIntersecting() && !r.boundingBox().intersects(q.intersectingValue())) {
                continue;
            }
            out.push_back(r);
        }
        return out;
    }

    std::vector<ObjectRecord> records_;
    ObjectQuery last_;
    bool lastCursorClosed_ = false;
};

} // namespace slideio::processor
