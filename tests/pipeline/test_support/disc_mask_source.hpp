// Requirements: UR-IMG-5, UR-STG-3
#pragma once

// A TileMaskSource for the restriction tests: a mask that is set inside a level-0
// circle. Public headers only.

#include <slideio/processor/core/flow.hpp>
#include <slideio/processor/core/raster_mask.hpp>

#include <atomic>
#include <cstdint>

namespace slideio::processor::testing_support {

/// Sets a mask pixel when the level-0 centre of the pixel, ((x + 0.5) * levelScale,
/// (y + 0.5) * levelScale), lies within `radius` of (centreX, centreY); clears every
/// other pixel of the region. Counts its fillMask calls. Thread-safe.
class DiscMaskSource : public TileMaskSource {
public:
    DiscMaskSource(double centreX, double centreY, double radius);
    void fillMask(RasterMask& out) override;
    /// How many times fillMask has been called.
    int64_t fillCount() const;
    /// The same test for one level-0 point, for expectations.
    bool contains(double level0X, double level0Y) const;

private:
    double centreX_;
    double centreY_;
    double radius_;
    std::atomic<int64_t> fills_{0};
};

} // namespace slideio::processor::testing_support
