// Requirements: UR-IMG-5, UR-STG-3
#include <pipeline/test_support/disc_mask_source.hpp>

namespace slideio::processor::testing_support {

DiscMaskSource::DiscMaskSource(double centreX, double centreY, double radius)
    : centreX_(centreX), centreY_(centreY), radius_(radius) {}

bool DiscMaskSource::contains(double level0X, double level0Y) const {
    const double dx = level0X - centreX_;
    const double dy = level0Y - centreY_;
    return dx * dx + dy * dy <= radius_ * radius_;
}

void DiscMaskSource::fillMask(RasterMask& out) {
    ++fills_;
    const Rect64 region = out.regionAtLevel();
    const double scale = out.levelScale();
    for (int64_t y = 0; y < region.height; ++y) {
        uint8_t* row = out.row(y);
        const double cy = (static_cast<double>(region.y + y) + 0.5) * scale;
        for (int64_t x = 0; x < region.width; ++x) {
            const double cx = (static_cast<double>(region.x + x) + 0.5) * scale;
            row[x] = contains(cx, cy) ? 255 : 0;
        }
    }
}

int64_t DiscMaskSource::fillCount() const { return fills_.load(); }

} // namespace slideio::processor::testing_support
