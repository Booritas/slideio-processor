// Requirements: UR-SEG-3, UR-QA-2, UR-QA-3
#pragma once

// A synthetic scene of blobs for the tiling-invariance tests: discs, annuli, a
// disc with two holes, U-shapes, squares touching only at a tile corner, and one
// tissue-like region that spans the whole level. Mask values vary inside every
// blob, so the mask measured as its own intensity has a non-trivial mean and
// deviation; an optional Float32 channel carries an intensity that varies quickly
// enough for every blob to have a well-conditioned deviation. Public headers only.

#include <pipeline/test_support/synthetic_scene.hpp>

#include <slideio/slideio/scene.hpp>

#include <cstdint>
#include <memory>
#include <vector>

namespace slideio::processor::testing_support {

/// Two squares of kCornerSquareSide pixels touching only at the level pixel
/// corner (x, y), a corner of the tile grid of `tileSide`-pixel tiles: one square
/// ends at (x, y), the other starts there. 4-connectivity keeps them two objects.
struct CornerTouchingPair {
    int32_t tileSide;
    int64_t x;
    int64_t y;
};

/// The side of each square of a corner-touching pair, in level pixels.
constexpr int64_t kCornerSquareSide = 20;

/// One pair per tile size the tests use (64, 100 and 257), each meeting at a
/// corner of that size's grid.
const std::vector<CornerTouchingPair>& cornerTouchingPairs();

/// What the scene's channels hold.
enum class BlobChannels {
    Mask,              ///< One Byte channel: the mask, 0 outside every blob, 1..255 inside.
    MaskAndFloatValues ///< Channel 0 the Byte mask, channel 1 a Float32 intensity everywhere.
};

/// The blob layout for a level of `width` x `height` pixels (at least 1000 x 800):
/// the blobs sit in the top-left 1000 x 800 pixels; the spanning region is a
/// frame along the level's border plus a wavy horizontal band near y = 400 and a
/// wavy vertical band near x = 500, each crossing the whole level, so it has four
/// holes, which hold the blobs. Row-major, one byte per pixel.
std::vector<uint8_t> blobMask(int64_t width, int64_t height);

/// The Float32 channel's value at level pixel (x, y), as the double it is cast from.
double blobIntensity(int64_t x, int64_t y);

/// A scene of the given pyramid whose level `blobLevel` holds the blob layout.
/// Other levels read the same function in their own coordinates (mask zero
/// beyond the blob level's extent); the tests analyse only the blob level.
std::shared_ptr<slideio::Scene> makeBlobScene(std::vector<SyntheticLevel> levels, int32_t blobLevel,
                                              BlobChannels channels);

} // namespace slideio::processor::testing_support
