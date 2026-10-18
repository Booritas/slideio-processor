#include <slideio/processor/core/object_record.hpp>

#include <utility>

namespace slideio::processor {

class ObjectRecord::Impl {
public:
    void reset() noexcept {
        geometry.clear();
        measurements.clear();
        layer = LayerId();
        stage = StageId();
        label = LabelId();
    }

    Polygon geometry;
    MeasurementSet measurements;
    LayerId layer;
    StageId stage;
    LabelId label;
};

namespace {
const Polygon& emptyPolygon() {
    static const Polygon empty;
    return empty;
}
const MeasurementSet& emptyMeasurements() {
    static const MeasurementSet empty;
    return empty;
}
} // namespace

ObjectRecord::ObjectRecord() : impl_(std::make_unique<Impl>()) {}

ObjectRecord::ObjectRecord(const ObjectRecord& other)
    : impl_(std::make_unique<Impl>(other.impl_ ? *other.impl_ : Impl())) {}

ObjectRecord::ObjectRecord(ObjectRecord&& other) noexcept : impl_(std::move(other.impl_)) {}

ObjectRecord& ObjectRecord::operator=(const ObjectRecord& other) {
    if (this != &other) {
        auto copy = std::make_unique<Impl>(other.impl_ ? *other.impl_ : Impl());
        impl_ = std::move(copy);
    }
    return *this;
}

ObjectRecord& ObjectRecord::operator=(ObjectRecord&& other) noexcept {
    if (this != &other) {
        impl_ = std::move(other.impl_); // never allocates; the source reads as default
    }
    return *this;
}

ObjectRecord::~ObjectRecord() = default;

ObjectRecord::Impl& ObjectRecord::ensure() {
    if (!impl_) {
        impl_ = std::make_unique<Impl>();
    }
    return *impl_;
}

ObjectId ObjectRecord::id() const noexcept {
    if (!impl_ || impl_->geometry.isEmpty() || !impl_->layer.isValid()) {
        return ObjectId();
    }
    return ObjectId::fromAnchor(impl_->layer, impl_->geometry.anchor());
}

LayerId ObjectRecord::layer() const noexcept { return impl_ ? impl_->layer : LayerId(); }

StageId ObjectRecord::stage() const noexcept { return impl_ ? impl_->stage : StageId(); }

const Polygon& ObjectRecord::geometry() const noexcept {
    return impl_ ? impl_->geometry : emptyPolygon();
}

Rect64 ObjectRecord::boundingBox() const noexcept { return geometry().boundingBox(); }

bool ObjectRecord::hasLabel() const noexcept { return label().isValid(); }

LabelId ObjectRecord::label() const noexcept { return impl_ ? impl_->label : LabelId(); }

const MeasurementSet& ObjectRecord::measurements() const noexcept {
    return impl_ ? impl_->measurements : emptyMeasurements();
}

MeasurementSet& ObjectRecord::measurements() noexcept { return ensure().measurements; }

void ObjectRecord::setGeometry(const Polygon& geometry) { ensure().geometry = geometry; }

void ObjectRecord::setLayer(LayerId layer) { ensure().layer = layer; }

void ObjectRecord::setLabel(LabelId label) { ensure().label = label; }

void ObjectRecord::clearLabel() noexcept {
    if (impl_) {
        impl_->label = LabelId();
    }
}

void ObjectRecord::reset() noexcept {
    if (impl_) {
        impl_->reset();
    }
}

} // namespace slideio::processor
