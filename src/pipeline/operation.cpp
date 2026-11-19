// Requirements: UR-EXT-1, UR-EXT-2, UR-EXT-3, UR-EXT-4, UR-SEG-3, UR-SEG-5, UR-SEG-7
#include <slideio/processor/pipeline/operation.hpp>

#include <slideio/processor/core/error.hpp>

#include <string>

namespace slideio::processor {

Halo Halo::uniform(int32_t n) {
    if (n < 0) {
        const std::string message = "Halo::uniform: the width must not be negative, got " + std::to_string(n);
        throw InvalidArgumentError(message.c_str());
    }
    return Halo{n, n, n, n};
}

Halo Halo::none() noexcept {
    return Halo{0, 0, 0, 0};
}

bool Halo::isEmpty() const noexcept {
    return left == 0 && top == 0 && right == 0 && bottom == 0;
}

Operation::~Operation() = default;

bool Operation::needsGlobalContext() const {
    return false;
}

void Operation::accumulate(const OperationContext&) {}

void Operation::combine(const Operation&) {}

void Operation::finalizeAccumulation() {}

TileRequirement Operation::tileRequirement() const {
    return TileRequirement::Halo;
}

int64_t Operation::workingSetBytesPerLevelPixel() const {
    return 0;
}

bool Operation::producesObjects() const {
    return false;
}

int32_t Operation::outputPlaneCount(int32_t inputPlaneCount) const {
    return inputPlaneCount;
}

int64_t Operation::workingSetBytesPerTile() const {
    return 0;
}

int64_t Operation::accumulatedStateBytes(int32_t) const {
    return 0;
}

bool Operation::finishObject(ObjectRecord&, const ObjectFinishContext&) const {
    return true;
}

bool Operation::objectsAreEightConnected() const {
    return false;
}

} // namespace slideio::processor
