#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/object_query.hpp>

#include <string>
#include <utility>
#include <vector>

namespace slideio::processor {

class ObjectQuery::Impl {
public:
    struct Bound {
        std::string name;
        double value;
        bool isLowerBound;
    };

    bool hasLayer = false;
    LayerId layer;
    bool hasStage = false;
    StageId stage;
    bool hasIntersecting = false;
    Rect64 intersecting{0, 0, 0, 0};
    bool hasPolygon = false;
    Polygon polygon;
    SpatialPredicate predicate = SpatialPredicate::Intersects;
    bool hasLabel = false;
    LabelId label;
    LabelFilter labelFilter = LabelFilter::Any;
    AttachmentFilter attachment = AttachmentFilter::Any;
    StageVisibility visibility = StageVisibility::CompletedOnly;
    bool hasLimit = false;
    int64_t limit = 0;
    std::vector<Bound> bounds;
};

ObjectQuery::ObjectQuery() : impl_(std::make_unique<Impl>()) {}

ObjectQuery::ObjectQuery(const ObjectQuery& other)
    : impl_(std::make_unique<Impl>(other.view())) {}

ObjectQuery::ObjectQuery(ObjectQuery&& other) noexcept : impl_(std::move(other.impl_)) {}

ObjectQuery& ObjectQuery::operator=(const ObjectQuery& other) {
    if (this != &other) {
        impl_ = std::make_unique<Impl>(other.view());
    }
    return *this;
}

ObjectQuery& ObjectQuery::operator=(ObjectQuery&& other) noexcept {
    if (this != &other) {
        impl_ = std::move(other.impl_);
    }
    return *this;
}

ObjectQuery::~ObjectQuery() = default;

ObjectQuery::Impl& ObjectQuery::ensure() {
    if (!impl_) {
        impl_ = std::make_unique<Impl>();
    }
    return *impl_;
}

// A moved-from query reads as a default query.
const ObjectQuery::Impl& ObjectQuery::view() const noexcept {
    static const Impl defaults;
    return impl_ ? *impl_ : defaults;
}

ObjectQuery& ObjectQuery::layer(LayerId layer) {
    Impl& i = ensure();
    i.hasLayer = true;
    i.layer = layer;
    return *this;
}

ObjectQuery& ObjectQuery::stage(StageId stage) {
    Impl& i = ensure();
    i.hasStage = true;
    i.stage = stage;
    return *this;
}

ObjectQuery& ObjectQuery::intersecting(const Rect64& rect) {
    Impl& i = ensure();
    i.hasIntersecting = true;
    i.intersecting = rect;
    return *this;
}

ObjectQuery& ObjectQuery::withinPolygon(const Polygon& polygon, SpatialPredicate predicate) {
    Impl& i = ensure();
    Polygon copy(polygon); // may throw before any state changes
    i.polygon = std::move(copy);
    i.hasPolygon = true;
    i.predicate = predicate;
    return *this;
}

ObjectQuery& ObjectQuery::label(LabelId label) {
    Impl& i = ensure();
    i.hasLabel = true;
    i.label = label;
    return *this;
}

ObjectQuery& ObjectQuery::labelFilter(LabelFilter filter) {
    ensure().labelFilter = filter;
    return *this;
}

ObjectQuery& ObjectQuery::attachment(AttachmentFilter filter) {
    ensure().attachment = filter;
    return *this;
}

ObjectQuery& ObjectQuery::measurementAtLeast(const char* name, double value) {
    ensure().bounds.push_back({name ? name : "", value, true});
    return *this;
}

ObjectQuery& ObjectQuery::measurementAtMost(const char* name, double value) {
    ensure().bounds.push_back({name ? name : "", value, false});
    return *this;
}

ObjectQuery& ObjectQuery::visibility(StageVisibility visibility) {
    ensure().visibility = visibility;
    return *this;
}

ObjectQuery& ObjectQuery::limit(int64_t maxObjects) {
    Impl& i = ensure();
    i.hasLimit = true;
    i.limit = maxObjects;
    return *this;
}

bool ObjectQuery::hasSpatialFilter() const noexcept {
    const Impl& i = view();
    return i.hasIntersecting || i.hasPolygon;
}

bool ObjectQuery::isUnfiltered() const noexcept {
    const Impl& i = view();
    return !i.hasLayer && !i.hasStage && !hasSpatialFilter() && !i.hasLabel &&
           i.labelFilter == LabelFilter::Any && i.attachment == AttachmentFilter::Any &&
           i.bounds.empty();
}

bool ObjectQuery::hasLayer() const noexcept { return view().hasLayer; }
LayerId ObjectQuery::layerValue() const noexcept { return view().layer; }
bool ObjectQuery::hasStage() const noexcept { return view().hasStage; }
StageId ObjectQuery::stageValue() const noexcept { return view().stage; }
bool ObjectQuery::hasIntersecting() const noexcept { return view().hasIntersecting; }
Rect64 ObjectQuery::intersectingValue() const noexcept { return view().intersecting; }
bool ObjectQuery::hasWithinPolygon() const noexcept { return view().hasPolygon; }
const Polygon& ObjectQuery::withinPolygonValue() const noexcept { return view().polygon; }
SpatialPredicate ObjectQuery::withinPolygonPredicate() const noexcept { return view().predicate; }
bool ObjectQuery::hasLabel() const noexcept { return view().hasLabel; }
LabelId ObjectQuery::labelValue() const noexcept { return view().label; }
LabelFilter ObjectQuery::labelFilterValue() const noexcept { return view().labelFilter; }
AttachmentFilter ObjectQuery::attachmentValue() const noexcept { return view().attachment; }
StageVisibility ObjectQuery::visibilityValue() const noexcept { return view().visibility; }
bool ObjectQuery::hasLimit() const noexcept { return view().hasLimit; }
int64_t ObjectQuery::limitValue() const noexcept { return view().limit; }

int32_t ObjectQuery::measurementBoundCount() const noexcept {
    return static_cast<int32_t>(view().bounds.size());
}

MeasurementBound ObjectQuery::measurementBoundAt(int32_t index) const {
    const Impl& i = view();
    if (index < 0 || static_cast<size_t>(index) >= i.bounds.size()) {
        throw InvalidArgumentError("measurement bound index out of range");
    }
    const Impl::Bound& b = i.bounds[static_cast<size_t>(index)];
    return MeasurementBound{b.name.c_str(), b.value, b.isLowerBound};
}

} // namespace slideio::processor
