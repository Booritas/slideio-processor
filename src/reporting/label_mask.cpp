// Requirements: UR-RES-7
#include <slideio/processor/reporting/label_mask.hpp>

#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/object_record.hpp>
#include <slideio/processor/storage/annotation_store.hpp>
#include <slideio/processor/storage/object_store.hpp>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <utility>

namespace slideio::processor {

namespace {

class EmptyCursor final : public Cursor {
public:
    bool next() override { return false; }
    const ObjectRecord& current() const override { throw InvalidArgumentError("cursor: there is no current object"); }
    int64_t position() const override { return -1; }
    void close() override {}
};

// Yields the inner cursor's records with each label replaced by the mask value.
class ValueCursor final : public Cursor {
public:
    ValueCursor(CursorPtr inner, const AnnotationStore& annotations, LabelId fixed)
        : inner_(std::move(inner)), annotations_(&annotations), fixed_(fixed) {}

    bool next() override {
        if (!inner_->next()) return false;
        record_ = inner_->current();
        if (fixed_.isValid()) {
            record_.setLabel(fixed_);
        } else if (annotations_->labelCount(record_.id()) > 0) {
            record_.setLabel(annotations_->labelAt(record_.id(), 0));
        } else {
            record_.clearLabel();
        }
        return true;
    }
    const ObjectRecord& current() const override { return record_; }
    int64_t position() const override { return inner_->position(); }
    void close() override { inner_->close(); }

private:
    CursorPtr inner_;
    const AnnotationStore* annotations_;
    LabelId fixed_;
    ObjectRecord record_;
};

// The store's objects as the query admits them: each tile query from core's rasteriser
// is merged into the caller's, and each record's label is the mask value.
class QueryObjects final : public ObjectSource {
public:
    QueryObjects(Store& store, const ObjectQuery& query) : store_(&store), query_(query) {}

    CursorPtr query(const ObjectQuery& tile) override {
        ObjectQuery merged;
        if (!merge(tile, merged)) return std::make_shared<EmptyCursor>();
        CursorPtr inner = store_->objects().query(merged);
        return std::make_shared<ValueCursor>(std::move(inner), store_->annotations(), fixedLabel());
    }

    int64_t count(const ObjectQuery& tile) const override {
        ObjectQuery merged;
        return merge(tile, merged) ? store_->objects().count(merged) : 0;
    }

    bool get(ObjectId, ObjectRecord&) const override {
        throw InternalError("label mask source: get is not supported");
    }

private:
    LabelId fixedLabel() const { return query_.hasLabel() ? query_.labelValue() : LabelId(); }

    // Builds the caller's query with the tile's box intersected in. False when the two
    // boxes do not overlap, so no object can satisfy both.
    // Every filter is copied by name, so a filter added to ObjectQuery must be added here
    // too (object_query.hpp says so at its setters); limit() alone is left out on purpose.
    bool merge(const ObjectQuery& tile, ObjectQuery& out) const {
        out.layer(query_.layerValue());
        if (query_.hasStage()) out.stage(query_.stageValue());
        if (query_.hasWithinPolygon()) out.withinPolygon(query_.withinPolygonValue(), query_.withinPolygonPredicate());
        if (query_.hasLabel()) out.label(query_.labelValue());
        out.labelFilter(query_.labelFilterValue());
        out.attachment(query_.attachmentValue());
        for (int32_t i = 0; i < query_.measurementBoundCount(); ++i) {
            const MeasurementBound bound = query_.measurementBoundAt(i);
            if (bound.isLowerBound) {
                out.measurementAtLeast(bound.name, bound.value);
            } else {
                out.measurementAtMost(bound.name, bound.value);
            }
        }
        out.visibility(query_.visibilityValue());
        if (!tile.hasIntersecting()) return true;
        Rect64 box = tile.intersectingValue();
        if (query_.hasIntersecting()) {
            const Rect64 own = query_.intersectingValue();
            const int64_t x0 = std::max(box.x, own.x);
            const int64_t y0 = std::max(box.y, own.y);
            const int64_t x1 = std::min(box.x + box.width, own.x + own.width);
            const int64_t y1 = std::min(box.y + box.height, own.y + own.height);
            if (x1 <= x0 || y1 <= y0) return false;
            box = Rect64{x0, y0, x1 - x0, y1 - y0};
        }
        out.intersecting(box);
        return true;
    }

    Store* store_;  // borrowed
    ObjectQuery query_;
};

// core's Label source over QueryObjects, which it owns.
class LabelMaskSource final : public TileRasterSource {
public:
    LabelMaskSource(Store& store, const ObjectQuery& query)
        : objects_(store, query),
          source_(makeLayerRasterSource(objects_, query.layerValue(), LayerRasterContent::Label)) {}

    void fillTile(BlockView& out) override { source_->fillTile(out); }
    DataType planeType() const override { return source_->planeType(); }
    int32_t planeCount() const override { return source_->planeCount(); }

private:
    QueryObjects objects_;  // declared first: source_ borrows it
    TileRasterSourcePtr source_;
};

} // namespace

TileRasterSourcePtr makeLabelMaskSource(Store& store, const ObjectQuery& query) {
    if (!query.hasLayer() || !query.layerValue().isValid()) {
        throw InvalidArgumentError("makeLabelMaskSource: the query needs a layer");
    }
    return std::make_shared<LabelMaskSource>(store, query);
}

} // namespace slideio::processor
