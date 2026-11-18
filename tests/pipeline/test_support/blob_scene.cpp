// Requirements: UR-SEG-3, UR-QA-2, UR-QA-3
#include <pipeline/test_support/blob_scene.hpp>

#include <cmath>
#include <stdexcept>
#include <utility>

namespace slideio::processor::testing_support {

namespace {

// Blob positions are pixel-corner coordinates: a pixel (x, y) is inside a round
// shape when its centre (x + 0.5, y + 0.5) is, so a shape centred on a tile
// corner is cut symmetrically by the two seams through it.
double distanceSquared(int64_t x, int64_t y, double cx, double cy) {
    const double dx = static_cast<double>(x) + 0.5 - cx;
    const double dy = static_cast<double>(y) + 0.5 - cy;
    return dx * dx + dy * dy;
}

bool inDisc(int64_t x, int64_t y, double cx, double cy, double r) { return distanceSquared(x, y, cx, cy) <= r * r; }

bool inRect(int64_t x, int64_t y, int64_t x0, int64_t y0, int64_t w, int64_t h) {
    return x >= x0 && x < x0 + w && y >= y0 && y < y0 + h;
}

// A U-shape: the rectangle (x0, y0, w, h) less an inner rectangle that leaves a
// wall of `t` pixels on three sides and is open on the fourth, `open` being
// 'u' (top), 'n' (bottom) or 'c' (left). Its arms are separate pieces in any tile
// that cuts them below the bottom wall, so they join only through another tile.
bool inU(int64_t x, int64_t y, int64_t x0, int64_t y0, int64_t w, int64_t h, int64_t t, char open) {
    if (!inRect(x, y, x0, y0, w, h)) {
        return false;
    }
    switch (open) {
    case 'u':
        return !inRect(x, y, x0 + t, y0, w - 2 * t, h - t);
    case 'n':
        return !inRect(x, y, x0 + t, y0 + t, w - 2 * t, h - t);
    default:
        return !inRect(x, y, x0, y0 + t, w - t, h - 2 * t);
    }
}

bool inRegion(int64_t width, int64_t height, int64_t x, int64_t y) {
    const int64_t frame = 8;
    if (x < frame || y < frame || x >= width - frame || y >= height - frame) {
        return true;
    }
    // The bands' slopes stay below one pixel per pixel and they are eight pixels
    // thick, so each is 4-connected.
    const double px = static_cast<double>(x) + 0.5;
    const double py = static_cast<double>(y) + 0.5;
    const double bandY = 400.0 + 20.0 * std::sin(px / 37.0);
    const double bandX = 500.0 + 15.0 * std::sin(py / 29.0);
    return std::abs(py - bandY) < 4.0 || std::abs(px - bandX) < 4.0;
}

bool inBlob(int64_t x, int64_t y) {
    // Annuli: (200, 200) is a corner of the 100-pixel grid, (768, 256) one of the
    // 64-pixel grid, (257, 514) one of the 257-pixel grid (and near one of the 64-pixel grid);
    // each is cut by four seams of its grid.
    const auto annulus = [&](double cx, double cy, double outer, double inner) {
        return inDisc(x, y, cx, cy, outer) && !inDisc(x, y, cx, cy, inner);
    };
    if (annulus(200, 200, 40, 22) || annulus(768, 256, 45, 25) || annulus(257, 514, 40, 22)) {
        return true;
    }
    // A disc with two holes, so a merged object orders more than one hole.
    if (inDisc(x, y, 650, 620, 50) && !inDisc(x, y, 630, 620, 12) && !inDisc(x, y, 672, 612, 12)) {
        return true;
    }
    // U-shapes opening up, down and left, their arms cut by seams of every grid.
    if (inU(x, y, 310, 230, 120, 120, 18, 'u') || inU(x, y, 800, 450, 140, 110, 18, 'n') ||
        inU(x, y, 60, 460, 120, 120, 18, 'c')) {
        return true;
    }
    // Discs, one of them tiny and centred on a 64-pixel tile corner.
    if (inDisc(x, y, 100, 330, 28) || inDisc(x, y, 360, 80, 22) || inDisc(x, y, 880, 130, 35) ||
        inDisc(x, y, 130, 720, 45) || inDisc(x, y, 420, 640, 20) || inDisc(x, y, 900, 720, 40) ||
        inDisc(x, y, 640, 448, 4)) {
        return true;
    }
    for (const CornerTouchingPair& pair : cornerTouchingPairs()) {
        if (inRect(x, y, pair.x - kCornerSquareSide, pair.y - kCornerSquareSide, kCornerSquareSide,
                   kCornerSquareSide) ||
            inRect(x, y, pair.x, pair.y, kCornerSquareSide, kCornerSquareSide)) {
            return true;
        }
    }
    return false;
}

} // namespace

const std::vector<CornerTouchingPair>& cornerTouchingPairs() {
    static const std::vector<CornerTouchingPair> pairs = {{64, 448, 64}, {100, 600, 100}, {257, 257, 257}};
    return pairs;
}

std::vector<uint8_t> blobMask(int64_t width, int64_t height) {
    if (width < 1000 || height < 800) {
        throw std::invalid_argument("blobMask: the level must be at least 1000 x 800 pixels");
    }
    std::vector<uint8_t> mask(static_cast<size_t>(width * height), 0);
    for (int64_t y = 0; y < height; ++y) {
        for (int64_t x = 0; x < width; ++x) {
            if (inRegion(width, height, x, y) || inBlob(x, y)) {
                // Never zero, so every blob pixel is foreground.
                mask[static_cast<size_t>(y * width + x)] = static_cast<uint8_t>(1 + (x * 7 + y * 13) % 255);
            }
        }
    }
    return mask;
}

double blobIntensity(int64_t x, int64_t y) {
    const double fx = static_cast<double>(x);
    const double fy = static_cast<double>(y);
    // Varies by tens within a few pixels, so even the smallest blob's deviation
    // is a sizeable fraction of its mean; the last term breaks any periodicity.
    return 100.0 + 80.0 * std::sin(0.9 * fx + 1.3 * fy) * std::cos(0.31 * fx) + 0.37 * static_cast<double>(x % 7);
}

std::shared_ptr<slideio::Scene> makeBlobScene(std::vector<SyntheticLevel> levels, int32_t blobLevel,
                                              BlobChannels channels) {
    if (blobLevel < 0 || static_cast<size_t>(blobLevel) >= levels.size()) {
        throw std::invalid_argument("makeBlobScene: no such level");
    }
    const int64_t width = levels[static_cast<size_t>(blobLevel)].width;
    const int64_t height = levels[static_cast<size_t>(blobLevel)].height;
    const auto mask = std::make_shared<const std::vector<uint8_t>>(blobMask(width, height));
    std::vector<slideio::DataType> types = {slideio::DataType::DT_Byte};
    if (channels == BlobChannels::MaskAndFloatValues) {
        types.push_back(slideio::DataType::DT_Float32);
    }
    PixelFunction pixel = [mask, width, height](int channel, int64_t x, int64_t y) {
        if (channel == 1) {
            return blobIntensity(x, y);
        }
        if (x < 0 || y < 0 || x >= width || y >= height) {
            return 0.0;
        }
        return static_cast<double>((*mask)[static_cast<size_t>(y * width + x)]);
    };
    return makeSyntheticScene(std::move(levels), std::move(types), std::move(pixel));
}

} // namespace slideio::processor::testing_support
