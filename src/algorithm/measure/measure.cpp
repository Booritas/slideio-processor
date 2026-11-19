// Requirements: UR-SEG-5, UR-SEG-6
#include <slideio/processor/algorithm/measure/measure.hpp>

#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/measurement.hpp>
#include <slideio/slideio/scene.hpp>

#include <memory>
#include <tuple>

namespace slideio::processor {

namespace {

constexpr int32_t kKindCount = 8;
constexpr uint32_t kAllKinds = (1u << kKindCount) - 1u;
constexpr uint32_t kGeometryKinds = (1u << static_cast<int32_t>(MeasurementKind::Area)) |
                                    (1u << static_cast<int32_t>(MeasurementKind::Perimeter)) |
                                    (1u << static_cast<int32_t>(MeasurementKind::Centroid)) |
                                    (1u << static_cast<int32_t>(MeasurementKind::BoundingBox));

bool isKind(MeasurementKind kind) noexcept {
    const auto value = static_cast<int32_t>(kind);
    return value >= 0 && value < kKindCount;
}

} // namespace

class MeasurementSelection::Impl {
public:
    uint32_t kinds = 0; // bit k set when MeasurementKind k is selected
};

MeasurementSelection::MeasurementSelection() = default;

MeasurementSelection::MeasurementSelection(const MeasurementSelection& other)
    : impl_(other.impl_ ? std::make_unique<Impl>(*other.impl_) : nullptr) {}

MeasurementSelection::MeasurementSelection(MeasurementSelection&& other) noexcept = default;

MeasurementSelection& MeasurementSelection::operator=(const MeasurementSelection& other) {
    if (this != &other) {
        std::unique_ptr<Impl> copy = other.impl_ ? std::make_unique<Impl>(*other.impl_) : nullptr;
        impl_ = std::move(copy);
    }
    return *this;
}

MeasurementSelection& MeasurementSelection::operator=(MeasurementSelection&& other) noexcept = default;

MeasurementSelection::~MeasurementSelection() = default;

MeasurementSelection MeasurementSelection::standard() {
    MeasurementSelection selection;
    selection.addAll();
    return selection;
}

MeasurementSelection MeasurementSelection::geometryOnly() {
    MeasurementSelection selection;
    selection.impl_ = std::make_unique<Impl>();
    selection.impl_->kinds = kGeometryKinds;
    return selection;
}

MeasurementSelection& MeasurementSelection::add(MeasurementKind kind) {
    if (!isKind(kind)) {
        throw InvalidArgumentError("MeasurementSelection: parameter 'kind' is not a MeasurementKind enumerator");
    }
    if (!impl_) {
        impl_ = std::make_unique<Impl>();
    }
    impl_->kinds |= 1u << static_cast<int32_t>(kind);
    return *this;
}

MeasurementSelection& MeasurementSelection::addAll() {
    if (!impl_) {
        impl_ = std::make_unique<Impl>();
    }
    impl_->kinds = kAllKinds;
    return *this;
}

bool MeasurementSelection::contains(MeasurementKind kind) const noexcept {
    return impl_ && isKind(kind) && (impl_->kinds & (1u << static_cast<int32_t>(kind))) != 0;
}

int32_t MeasurementSelection::count() const noexcept {
    int32_t n = 0;
    for (int32_t k = 0; impl_ && k < kKindCount; ++k) {
        n += (impl_->kinds >> k) & 1u;
    }
    return n;
}

double scenePixelSizeMicrons(std::shared_ptr<slideio::Scene> scene) noexcept {
    if (!scene) {
        return 0.0;
    }
    try {
        const std::tuple<double, double> resolution = scene->getResolution();
        return pixelSizeMicronsFromResolution(std::get<0>(resolution), std::get<1>(resolution));
    } catch (...) {
        return 0.0;
    }
}

bool sceneIsCalibrated(std::shared_ptr<slideio::Scene> scene) noexcept {
    return scenePixelSizeMicrons(std::move(scene)) > 0.0;
}

} // namespace slideio::processor
