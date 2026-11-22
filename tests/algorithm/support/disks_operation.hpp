// Requirements: UR-SEG-3, UR-EXT-4, UR-QA-3
#pragma once

// Test-only: "test.disks", a LabelImageOperation whose "model" draws filled disks
// and rings at fixed level-0 centres and labels them as a segmentation network
// would: the labels depend on the tile, so one disk carries different values in
// different tiles. Linked by the algorithm tests and by later tests that run a
// LabelImageOperation end to end (tests/run).

#include <slideio/processor/algorithm/segment/label_image.hpp>
#include <slideio/processor/core/geometry.hpp>
#include <slideio/processor/pipeline/operation_parameters.hpp>

#include <cstdint>
#include <vector>

namespace slideio::processor::testing_support {

/// A disk, or a ring when innerRadius is positive, in level-0 pixels. A level
/// pixel belongs to it when its centre, mapped to level 0, lies at a distance
/// d from (cx, cy) with innerRadius <= d <= radius (d >= innerRadius only when
/// innerRadius is positive).
struct Disk {
    double cx = 0.0;
    double cy = 0.0;
    double radius = 0.0;
    double innerRadius = 0.0;
};

/// What "test.disks" draws: its parameters, decoded.
struct DisksSpec {
    int32_t halo = 32;
    std::vector<Disk> disks;
    /// When both are valid disk indices, disk sharedSecond takes disk sharedFirst's
    /// label in every tile, so two objects share one value.
    int32_t sharedFirst = -1;
    int32_t sharedSecond = -1;
};

/// The parameter bag of "test.disks":
/// - "halo" (int, default 32): the halo on every side, in level pixels;
/// - "disks" (double array, default empty): cx, cy, radius, innerRadius per disk;
/// - "sharedFirst", "sharedSecond" (int, default -1): see DisksSpec.
OperationParameters disksParameters(const DisksSpec& spec);

/// Decodes a bag; throws InvalidArgumentError on a negative halo or a "disks"
/// array whose length is not a multiple of 4.
DisksSpec disksSpecOf(const OperationParameters& parameters);

/// The label test.disks predicts at level pixel (x, y) when the tile's rect at
/// the level is @p tile: 0 outside every disk; otherwise that of the last disk
/// drawn there, later disks drawn over earlier ones. Disk i gets a value from a
/// rotation of the disks that depends on the tile, so the values of the disks
/// differ between tiles but stay distinct within one, except for the shared pair.
int32_t diskLabelAt(const DisksSpec& spec, const Rect64& tile, double levelScale, int64_t x, int64_t y);

/// "test.disks": predicts diskLabelAt() over the halo rect, ignoring its input.
class DisksOperation : public LabelImageOperation {
public:
    explicit DisksOperation(const OperationParameters& parameters);
    const char* name() const override;
    Halo halo() const override;
    const OperationParameters& parameters() const override;
    void predictLabels(const OperationContext& context, BlockView& labels) override;

private:
    OperationParameters parameters_;
    DisksSpec spec_;
};

/// Registers "test.disks" with the catalog unless it is registered already.
void registerDisksOperation();

/// How many times predictLabels() of any DisksOperation has been called in this
/// process.
int64_t disksPredictCount();

} // namespace slideio::processor::testing_support
