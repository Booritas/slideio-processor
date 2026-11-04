#include "storage/detail/query_builder.hpp"
#include "storage/detail/sqlite.hpp"
#include "storage/detail/store_impl.hpp"

#include <slideio/processor/core/block_view.hpp>
#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/flow.hpp>
#include <slideio/processor/core/raster_mask.hpp>
#include <slideio/processor/storage/types.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace slideio::processor::detail {

namespace {

constexpr const char* kOperation = "AnnotationStore::layerRasterSource: fillTile";

// The effective label (ruling S15): the first member of the effective set that
// annotation_store.cpp defines, the row label, else the smallest asserted label whose
// stage has the bound status, else 0. Spliced in after kObjectColumns, so its one
// parameter is the statement's first.
constexpr const char* kEffectiveLabelColumn =
    ", COALESCE(o.label_id, (SELECT MIN(a.label_id) FROM label_assertions a JOIN stages sa ON sa.id = a.stage_id "
    "WHERE a.object_rowid = o.rowid AND sa.status = ?), 0)";
constexpr int kEffectiveLabelIndex = 6;  // after the six kObjectColumns

// The grid, the query extent and the padding below repeat core's layer raster source
// (layer_sources.cpp), so this source reads exactly the objects core's would. core's
// gridOfView is private to core (design 4.1), so the grid is recomputed here from the
// view's public geometry, as rasteriseInto(BlockView&, ...) derives it.

constexpr double kMaxExtent = 9.2e18;  // safely inside int64

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

// [x0, x1) x [y0, y1) grown by one level-0 pixel on each side, saturating at the int64
// limits; an extent whose width or height does not fit int64 is rejected.
Rect64 paddedQuery(int64_t x0, int64_t y0, int64_t x1, int64_t y1) {
    constexpr int64_t kMin = std::numeric_limits<int64_t>::min();
    constexpr int64_t kMax = std::numeric_limits<int64_t>::max();
    x0 = x0 > kMin ? x0 - 1 : x0;
    y0 = y0 > kMin ? y0 - 1 : y0;
    x1 = x1 < kMax ? x1 + 1 : x1;
    y1 = y1 < kMax ? y1 + 1 : y1;
    if ((x0 < 0 && x1 > kMax + x0) || (y0 < 0 && y1 > kMax + y0)) {
        throw InvalidArgumentError("layer source: query extent overflows int64");
    }
    return Rect64{x0, y0, x1 - x0, y1 - y0};
}

// The level-0 rectangle covered by the grid rasteriseInto draws `view` on: pixel size
// slideRect / size, origin llround(slideRect.x / pixelW) (likewise y), so the query
// covers what is rasterised even where the rounded grid extends past slideRect.
Rect64 queryOfView(const BlockView& view) {
    const Rect64 rect = view.slideRect();
    const double sx = static_cast<double>(rect.width) / static_cast<double>(view.width());
    const double sy = static_cast<double>(rect.height) / static_cast<double>(view.height());
    constexpr double kMinLevelScale = 1e-6;  // as rasteriseInto
    if (!std::isfinite(sx) || !std::isfinite(sy) || !(sx >= kMinLevelScale) || !(sy >= kMinLevelScale)) {
        throw InvalidArgumentError("fillTile: view scale must be finite and at least 1e-6");
    }
    const double ox = std::round(static_cast<double>(rect.x) / sx);
    const double oy = std::round(static_cast<double>(rect.y) / sy);
    constexpr double kOriginLimit = 4611686018427387904.0;  // 2^62
    if (!(std::fabs(ox) < kOriginLimit) || !(std::fabs(oy) < kOriginLimit)) {
        throw InvalidArgumentError("fillTile: view origin does not fit");
    }
    const int64_t originX = static_cast<int64_t>(ox), originY = static_cast<int64_t>(oy);
    if (originX > std::numeric_limits<int64_t>::max() - view.width() ||
        originY > std::numeric_limits<int64_t>::max() - view.height()) {
        throw InvalidArgumentError("fillTile: view origin does not fit");
    }
    return paddedQuery(toInt64Floor(static_cast<double>(originX) * sx), toInt64Floor(static_cast<double>(originY) * sy),
                       toInt64Ceil(static_cast<double>(originX + view.width()) * sx),
                       toInt64Ceil(static_cast<double>(originY + view.height()) * sy));
}

// One object to rasterise.
struct LabelledShape {
    ObjectId id;
    Polygon geometry;
    int64_t label = 0;
};

class LabelRasterSource final : public TileRasterSource {
public:
    LabelRasterSource(std::shared_ptr<StoreGate> gate, LayerId layer) : gate_(std::move(gate)), layer_(layer) {}

    void fillTile(BlockView& out) override {
        if (out.isNull() || out.planeCount() < 1 || out.planeType(0) != DataType::Int32) {
            throw InvalidArgumentError("fillTile: view needs plane 0 of the source's plane type");
        }
        ObjectQuery query;
        query.layer(layer_).intersecting(queryOfView(out)).visibility(StageVisibility::CompletedOnly);
        BuiltQuery built = buildObjectQuery(query, kOperation);
        const std::string select = std::string("SELECT ") + kObjectColumns;
        if (built.rowsSql.compare(0, select.size(), select) != 0) {
            throw InternalError("AnnotationStore::layerRasterSource: unexpected object query");
        }
        built.rowsSql.insert(select.size(), kEffectiveLabelColumn);
        QueryValue complete;
        complete.integer = static_cast<int64_t>(StageStatus::Complete);
        built.values.insert(built.values.begin(), std::move(complete));

        std::vector<LabelledShape> shapes = gate_->with(kOperation, [&](StoreImpl& store) {
            return store.withReadConnection(kOperation, [&](sqlite3* db) {
                // One snapshot; the pool rolls the transaction back if anything throws.
                exec(db, "BEGIN");
                std::vector<LabelledShape> rows;
                {
                    Statement statement(db, built.rowsSql.c_str());
                    bindValues(statement, built.values);
                    while (statement.step()) {
                        LabelledShape shape;
                        decodeRowGeometry(statement, shape.geometry);
                        shape.id = ObjectId::fromAnchor(layer_, shape.geometry.anchor());
                        shape.label = statement.columnInt64(kEffectiveLabelIndex);
                        rows.push_back(std::move(shape));
                    }
                }
                exec(db, "COMMIT");
                return rows;
            });
        });

        const size_t rowBytes = static_cast<size_t>(out.width()) * sizeof(int32_t);
        auto* base = static_cast<uint8_t*>(out.planeData(0));
        const int64_t stride = out.planeRowStrideBytes(0);
        for (int64_t y = 0; y < out.height(); ++y) {
            std::memset(base + y * stride, 0, rowBytes);
        }
        // Descending id: the smallest id is written last and wins, as in core.
        std::stable_sort(shapes.begin(), shapes.end(),
                         [](const LabelledShape& a, const LabelledShape& b) { return b.id < a.id; });
        for (const LabelledShape& shape : shapes) {
            rasteriseInto(out, 0, shape.geometry, shape.label);
        }
    }

    DataType planeType() const override { return DataType::Int32; }

    int32_t planeCount() const override { return 1; }

private:
    std::shared_ptr<StoreGate> gate_;  // the store is borrowed (C-8) and reached through the gate
    LayerId layer_;
};

} // namespace

TileRasterSourcePtr makeLabelRasterSource(std::shared_ptr<StoreGate> gate, LayerId layer) {
    return std::make_shared<LabelRasterSource>(std::move(gate), layer);
}

} // namespace slideio::processor::detail
