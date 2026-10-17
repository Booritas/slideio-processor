#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/flow.hpp>
#include <slideio/processor/core/raster_mask.hpp>

#include "core/detail/coverage.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <new>
#include <vector>

namespace slideio::processor {

namespace {

// Closes a cursor on every exit path; a close() failure while unwinding is swallowed.
class CursorCloser {
public:
    explicit CursorCloser(Cursor& cursor) : cursor_(&cursor) {}
    CursorCloser(const CursorCloser&) = delete;
    CursorCloser& operator=(const CursorCloser&) = delete;
    ~CursorCloser() {
        if (cursor_) {
            try {
                cursor_->close();
            } catch (...) {
            }
        }
    }
    // Normal path: close and let any failure propagate.
    void close() {
        Cursor* c = cursor_;
        cursor_ = nullptr;
        c->close();
    }

private:
    Cursor* cursor_;
};

// Drains the cursor of `query` into a vector of records. ProcessorError and
// std::bad_alloc pass through; any other exception from the (foreign) source
// is translated to InternalError with what() as the cause (C-6).
std::vector<ObjectRecord> collect(ObjectSource& source, const ObjectQuery& query) {
    try {
        std::vector<ObjectRecord> records;
        CursorPtr cursor = source.query(query);
        if (!cursor) {
            throw InternalError("ObjectSource::query returned a null cursor");
        }
        CursorCloser closer(*cursor);
        while (cursor->next()) {
            records.push_back(cursor->current());
        }
        closer.close();
        return records;
    } catch (const ProcessorError&) {
        throw;
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::exception& e) {
        ErrorContext ctx;
        ctx.cause = e.what();
        throw InternalError("layer source: the ObjectSource failed", ctx);
    }
}

constexpr double kMaxExtent = 9.2e18; // safely inside int64

int64_t toInt64Floor(double v) {
    const double f = std::floor(v);
    if (!(f >= -kMaxExtent && f <= kMaxExtent)) {
        throw InvalidArgumentError("layer source: tile extent is outside the supported range");
    }
    return static_cast<int64_t>(f);
}

int64_t toInt64Ceil(double v) {
    const double f = std::ceil(v);
    if (!(f >= -kMaxExtent && f <= kMaxExtent)) {
        throw InvalidArgumentError("layer source: tile extent is outside the supported range");
    }
    return static_cast<int64_t>(f);
}

// The level-0 query rectangle [x0, x1) x [y0, y1), grown by one level-0 pixel
// on each side so that a sliver created by rounding the level grid is never
// excluded. Growth saturates at the int64 limits; an extent whose width or
// height does not fit int64 is rejected.
Rect64 paddedQuery(int64_t x0, int64_t y0, int64_t x1, int64_t y1) {
    constexpr int64_t kMin = std::numeric_limits<int64_t>::min();
    constexpr int64_t kMax = std::numeric_limits<int64_t>::max();
    x0 = x0 > kMin ? x0 - 1 : x0;
    y0 = y0 > kMin ? y0 - 1 : y0;
    x1 = x1 < kMax ? x1 + 1 : x1;
    y1 = y1 < kMax ? y1 + 1 : y1;
    // x1 - x0 overflows iff x0 < 0 and x1 > kMax + x0 (x1 >= x0 here).
    if ((x0 < 0 && x1 > kMax + x0) || (y0 < 0 && y1 > kMax + y0)) {
        throw InvalidArgumentError("layer source: query extent overflows int64");
    }
    return Rect64{x0, y0, x1 - x0, y1 - y0};
}

class LayerMaskSource final : public TileMaskSource {
public:
    LayerMaskSource(ObjectSource& source, LayerId layer, double dilation)
        : source_(&source), layer_(layer), dilation_(dilation) {}

    void fillMask(RasterMask& out) override {
        if (out.isNull()) {
            throw InvalidArgumentError("fillMask: mask is null");
        }
        const Rect64 region = out.regionAtLevel();
        const double scale = out.levelScale();

        // Radius in level pixels, saturated to int32.
        const double rd = std::ceil(dilation_ / scale);
        const int32_t r = rd >= static_cast<double>(std::numeric_limits<int32_t>::max())
                              ? std::numeric_limits<int32_t>::max()
                              : static_cast<int32_t>(rd);

        constexpr int64_t kMin = std::numeric_limits<int64_t>::min();
        constexpr int64_t kMax = std::numeric_limits<int64_t>::max();
        // The region is non-empty and x + width does not overflow (RasterMask invariants).
        const int64_t farX = region.x + region.width;
        const int64_t farY = region.y + region.height;
        if (region.x < kMin + r || region.y < kMin + r || farX > kMax - r || farY > kMax - r ||
            region.width > kMax - 2 * int64_t{r} || region.height > kMax - 2 * int64_t{r}) {
            throw InvalidArgumentError("fillMask: dilated region overflows int64");
        }
        const Rect64 scratchRegion{region.x - r, region.y - r, region.width + 2 * int64_t{r},
                                   region.height + 2 * int64_t{r}};

        const int64_t x0 = toInt64Floor(static_cast<double>(scratchRegion.x) * scale);
        const int64_t y0 = toInt64Floor(static_cast<double>(scratchRegion.y) * scale);
        const int64_t x1 =
            toInt64Ceil(static_cast<double>(scratchRegion.x + scratchRegion.width) * scale);
        const int64_t y1 =
            toInt64Ceil(static_cast<double>(scratchRegion.y + scratchRegion.height) * scale);
        const Rect64 level0 = paddedQuery(x0, y0, x1, y1);

        ObjectQuery query;
        query.layer(layer_).intersecting(level0).visibility(StageVisibility::CompletedOnly);
        const std::vector<ObjectRecord> records = collect(*source_, query);

        // dilateMask stays inside its region, so dilate a scratch mask with a margin of r.
        RasterMask scratch(scratchRegion, out.level(), scale);
        for (const ObjectRecord& record : records) {
            rasteriseInto(scratch, record.geometry());
        }
        dilateMask(scratch, r);

        const size_t width = static_cast<size_t>(region.width);
        for (int64_t j = 0; j < region.height; ++j) {
            std::memcpy(out.row(j), scratch.row(j + r) + r, width);
        }
    }

private:
    ObjectSource* source_; // borrowed (C-8); the pointee must outlive this object
    LayerId layer_;
    double dilation_;
};

class LayerRasterSource final : public TileRasterSource {
public:
    LayerRasterSource(ObjectSource& source, LayerId layer, LayerRasterContent content)
        : source_(&source), layer_(layer), content_(content) {}

    void fillTile(BlockView& out) override {
        if (out.isNull() || out.planeCount() < 1 || out.planeType(0) != planeType()) {
            throw InvalidArgumentError("fillTile: view needs plane 0 of the source's plane type");
        }
        ObjectQuery query;
        // Query the whole rasterised grid, which can extend past slideRect by half a pixel.
        const detail::PixelGrid grid = detail::gridOfView(out);
        const int64_t qx0 = toInt64Floor(static_cast<double>(grid.originLevelX) * grid.pixelW);
        const int64_t qy0 = toInt64Floor(static_cast<double>(grid.originLevelY) * grid.pixelH);
        const int64_t qx1 = toInt64Ceil(static_cast<double>(grid.originLevelX + grid.width) * grid.pixelW);
        const int64_t qy1 = toInt64Ceil(static_cast<double>(grid.originLevelY + grid.height) * grid.pixelH);
        query.layer(layer_)
            .intersecting(paddedQuery(qx0, qy0, qx1, qy1))
            .visibility(StageVisibility::CompletedOnly);
        std::vector<ObjectRecord> records = collect(*source_, query);

        const size_t rowBytes = static_cast<size_t>(out.width()) *
                                static_cast<size_t>(dataTypeSizeBytes(planeType()));
        auto* base = static_cast<uint8_t*>(out.planeData(0));
        const int64_t stride = out.planeRowStrideBytes(0);
        // Background: 0 for Label and Membership; -1 ("no object") for ObjectAnchor,
        // because 0 is a valid packed anchor. All-ones bytes are int64 -1.
        const int fill = content_ == LayerRasterContent::ObjectAnchor ? 0xFF : 0;
        for (int64_t y = 0; y < out.height(); ++y) {
            std::memset(base + y * stride, fill, rowBytes);
        }

        // Descending id: the smallest id is written last and wins (R11).
        std::stable_sort(records.begin(), records.end(),
                  [](const ObjectRecord& a, const ObjectRecord& b) { return b.id() < a.id(); });
        for (const ObjectRecord& record : records) {
            int64_t value = 1;
            if (content_ == LayerRasterContent::Label) {
                value = record.hasLabel() ? record.label().value() : 0;
            } else if (content_ == LayerRasterContent::ObjectAnchor) {
                const ObjectId id = record.id();
                if (!id.isValid()) {
                    continue; // its anchorPacked() would be -1, the "no object" value
                }
                value = id.anchorPacked();
            }
            rasteriseInto(out, 0, record.geometry(), value);
        }
    }

    DataType planeType() const override {
        switch (content_) {
        case LayerRasterContent::Membership:
            return DataType::Byte;
        case LayerRasterContent::ObjectAnchor:
            return DataType::Int64;
        case LayerRasterContent::Label:
        default:
            return DataType::Int32;
        }
    }

    int32_t planeCount() const override { return 1; }

private:
    ObjectSource* source_; // borrowed (C-8); the pointee must outlive this object
    LayerId layer_;
    LayerRasterContent content_;
};

} // namespace

TileMaskSourcePtr makeLayerMaskSource(ObjectSource& source, LayerId layer,
                                      double dilationLevel0Pixels) {
    if (!layer.isValid()) {
        throw InvalidArgumentError("makeLayerMaskSource: layer is invalid");
    }
    if (!std::isfinite(dilationLevel0Pixels) || dilationLevel0Pixels < 0.0) {
        throw InvalidArgumentError("makeLayerMaskSource: dilation must be finite and >= 0");
    }
    return std::make_shared<LayerMaskSource>(source, layer, dilationLevel0Pixels);
}

TileRasterSourcePtr makeLayerRasterSource(ObjectSource& source, LayerId layer,
                                          LayerRasterContent content) {
    if (!layer.isValid()) {
        throw InvalidArgumentError("makeLayerRasterSource: layer is invalid");
    }
    if (content != LayerRasterContent::Label && content != LayerRasterContent::Membership &&
        content != LayerRasterContent::ObjectAnchor) {
        throw InvalidArgumentError("makeLayerRasterSource: unknown content");
    }
    return std::make_shared<LayerRasterSource>(source, layer, content);
}

} // namespace slideio::processor
