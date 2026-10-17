#include <slideio/processor/core/object_record.hpp>

#include <utility>

namespace slideio::processor {

ObjectRecord::ObjectRecord() = default;

ObjectRecord::ObjectRecord(const ObjectRecord& other) = default;

ObjectRecord::ObjectRecord(ObjectRecord&& other) noexcept
    : geometry_(std::move(other.geometry_)),
      measurements_(std::move(other.measurements_)),
      layer_(other.layer_),
      stage_(other.stage_),
      label_(other.label_) {
    other.reset();
}

ObjectRecord& ObjectRecord::operator=(const ObjectRecord& other) {
    if (this != &other) {
        ObjectRecord copy(other);
        *this = std::move(copy);
    }
    return *this;
}

ObjectRecord& ObjectRecord::operator=(ObjectRecord&& other) noexcept {
    if (this != &other) {
        geometry_ = std::move(other.geometry_);
        measurements_ = std::move(other.measurements_);
        layer_ = other.layer_;
        stage_ = other.stage_;
        label_ = other.label_;
        other.reset();
    }
    return *this;
}

ObjectRecord::~ObjectRecord() = default;

ObjectId ObjectRecord::id() const noexcept {
    if (geometry_.isEmpty() || !layer_.isValid()) {
        return ObjectId();
    }
    return ObjectId::fromAnchor(layer_, geometry_.anchor());
}

LayerId ObjectRecord::layer() const noexcept { return layer_; }

StageId ObjectRecord::stage() const noexcept { return stage_; }

const Polygon& ObjectRecord::geometry() const noexcept { return geometry_; }

Rect64 ObjectRecord::boundingBox() const noexcept { return geometry_.boundingBox(); }

bool ObjectRecord::hasLabel() const noexcept { return label_.isValid(); }

LabelId ObjectRecord::label() const noexcept { return label_; }

const MeasurementSet& ObjectRecord::measurements() const noexcept { return measurements_; }

MeasurementSet& ObjectRecord::measurements() noexcept { return measurements_; }

void ObjectRecord::setGeometry(const Polygon& geometry) { geometry_ = geometry; }

void ObjectRecord::setLayer(LayerId layer) { layer_ = layer; }

void ObjectRecord::setLabel(LabelId label) { label_ = label; }

void ObjectRecord::clearLabel() noexcept { label_ = LabelId(); }

void ObjectRecord::reset() noexcept {
    geometry_.clear();
    measurements_.clear();
    layer_ = LayerId();
    stage_ = StageId();
    label_ = LabelId();
}

} // namespace slideio::processor
