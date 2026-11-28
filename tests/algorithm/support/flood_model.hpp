// Requirements: UR-SEG-8, UR-QA-2, UR-QA-3
#pragma once

// A whole-image reference model of segment.watershed's flood, written from SDS 06
// "Watershed, exactly" rather than from the operation: the yardstick the tile
// invariance harness compares the operation with. It runs on the whole level as
// one context (no tiles, no halo) and is deliberately simple, O(n^2) per image.

#include <cstdint>
#include <optional>
#include <vector>

namespace slideio::processor::testing_support {

/// What happens to foreground the reach check would refuse.
enum class ModelRule {
    Fail,       ///< The run is refused (ModelResult::refused).
    DropPlain,  ///< Such pixels are cleared after the flood (the operation's Drop).
    DropBounded ///< As DropPlain, and a seed may not claim a pixel beyond the spacing.
};

struct ModelParameters {
    int64_t markerDistance = 5;            // minimumMarkerDistancePx
    int64_t spacing = 32;                  // maximumMarkerSpacingPx
    std::optional<double> floor;           // minimumMarkerValue
    std::vector<int64_t> anchors;          // empty: LocalMaxima; else per-pixel value, -1 = none
    ModelRule rule = ModelRule::Fail;
};

struct ModelResult {
    bool refused = false;                  // Fail: some foreground beyond reach
    std::vector<int64_t> key;              // per pixel: basin key, or kModelBackground / kModelDropped
};

constexpr int64_t kModelBackground = -1;
constexpr int64_t kModelDropped = -2;

/// Floods @p values (width * height samples, row-major, level coordinates) as
/// segment.watershed defines it. A basin's key is `(y << 31) | x` of its marker
/// pixel, or with anchors the anchor value. Under Fail the key map is the flood
/// as it stands (a pixel no seed reached is kModelDropped, one reached from too
/// far keeps its key) and refused says whether the run would fail.
ModelResult modelWatershed(const std::vector<double>& values, int64_t width, int64_t height,
                           const ModelParameters& p);

} // namespace slideio::processor::testing_support
