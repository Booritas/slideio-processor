// Requirements: UR-EXE-2, UR-ALG-8, UR-SEG-1, UR-SEG-2, UR-SEG-8
#include <slideio/processor/algorithm/segment/segment.hpp>
#include <slideio/processor/pipeline/operation_catalog.hpp>
#include <slideio/processor/pipeline/operation_parameters.hpp>

#include <algorithm/detail/sample_codec.hpp>
#include <algorithm/support/working_set_check.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

using namespace slideio::processor;
namespace ts = slideio::processor::testing_support;

namespace {

using ts::expectApplyBounded;

// A plane-0 mask over the halo'd tile, by halo'd pixel coordinates.
struct Mask {
    const char* name;
    std::function<bool(int64_t x, int64_t y)> foreground;
};

// Masks that maximise what segmentation allocates: the most regions (one per
// pixel of a checkerboard or of a sparse grid), the most rings (a solid region
// with a hole at every other pixel), the most vertices per region (stripes,
// diagonals) and the trivial ones.
const std::vector<Mask>& adversarialMasks() {
    static const std::vector<Mask> masks = {
        {"checkerboard", [](int64_t x, int64_t y) { return (x + y) % 2 == 0; }},
        {"isolated pixels", [](int64_t x, int64_t y) { return x % 2 == 0 && y % 2 == 0; }},
        {"one-pixel holes", [](int64_t x, int64_t y) { return !(x % 2 == 1 && y % 2 == 1); }},
        {"rows", [](int64_t, int64_t y) { return y % 2 == 0; }},
        {"columns", [](int64_t x, int64_t) { return x % 2 == 0; }},
        {"diagonals", [](int64_t x, int64_t y) { return (x + y) % 3 == 0; }},
        {"joined diagonals", [](int64_t x, int64_t y) { return (x + y) % 3 == 0 || x == 1; }},
        {"comb", [](int64_t x, int64_t y) { return y % 2 == 0 || x == 1; }},
        {"staircase", [](int64_t x, int64_t y) { return (x / 2 + y) % 2 == 0; }},
        {"solid", [](int64_t, int64_t) { return true; }},
        {"empty", [](int64_t, int64_t) { return false; }},
    };
    return masks;
}

// Plane 0 as the mask, then @p intensityPlanes Float64 planes. With
// @p interior, the mask is cleared within 3 pixels of the edge, so no object
// touches the tile's edge: each is a finished object, record and all, rather
// than a seam fragment.
std::vector<std::vector<uint8_t>> planesOf(const Mask& mask, int64_t width, int64_t height, int32_t intensityPlanes,
                                           bool interior) {
    std::vector<std::vector<uint8_t>> planes;
    std::vector<uint8_t> plane0(static_cast<size_t>(width * height));
    for (int64_t y = 0; y < height; ++y) {
        for (int64_t x = 0; x < width; ++x) {
            const bool inside = !interior || (x >= 3 && y >= 3 && x < width - 3 && y < height - 3);
            plane0[static_cast<size_t>(y * width + x)] = inside && mask.foreground(x, y) ? 1 : 0;
        }
    }
    planes.push_back(std::move(plane0));
    for (int32_t p = 0; p < intensityPlanes; ++p) {
        std::vector<uint8_t> plane(static_cast<size_t>(width * height) * sizeof(double));
        for (int64_t i = 0; i < width * height; ++i) {
            const double v = static_cast<double>((i * 7 + p) % 101);
            std::memcpy(&plane[static_cast<size_t>(i) * sizeof(double)], &v, sizeof(double));
        }
        planes.push_back(std::move(plane));
    }
    return planes;
}

OperationPtr connectedComponents(Connectivity connectivity, bool extractHoles) {
    return makeConnectedComponents(
        ConnectedComponentsParameters().connectivity(connectivity).extractHoles(extractHoles));
}

} // namespace

TEST(WorkingSetSegment, ConnectedComponentsBoundedOnThePattern) {
    for (const Connectivity connectivity : {Connectivity::FourConnected, Connectivity::EightConnected}) {
        const auto make = [connectivity] { return connectedComponents(connectivity, true); };
        expectApplyBounded("segment.connected_components", make, {DataType::Byte});
        expectApplyBounded("segment.connected_components", make, {DataType::Byte, DataType::Float32});
        expectApplyBounded("segment.connected_components", make,
                           {DataType::UInt16, DataType::Float64, DataType::Int8, DataType::UInt64});
        for (const DataType type : ts::kAllDataTypes) {
            expectApplyBounded("segment.connected_components", make, {type});
        }
    }
}

// The declaration covers the worst masks with up to three measured planes,
// each object emitted as a seam fragment (the masks reach the tile's edge) or
// as a finished object (the masks kept inside).
TEST(WorkingSetSegment, ConnectedComponentsBoundedOnAdversarialMasks) {
    for (const int64_t tile : ts::kWorkingSetTileSizes) {
        const int64_t haloSide = tile;
        for (const Mask& mask : adversarialMasks()) {
            for (const bool interior : {false, true}) {
                for (const int32_t intensityPlanes : {0, 1, 3}) {
                    std::vector<DataType> types{DataType::Byte};
                    types.insert(types.end(), static_cast<size_t>(intensityPlanes), DataType::Float64);
                    const std::vector<std::vector<uint8_t>> planes =
                        planesOf(mask, haloSide, haloSide, intensityPlanes, interior);
                    for (const Connectivity connectivity :
                         {Connectivity::FourConnected, Connectivity::EightConnected}) {
                        for (const bool extractHoles : {true, false}) {
                            const OperationPtr op = connectedComponents(connectivity, extractHoles);
                            const int64_t peak = ts::measureWorkingSetPeakOn(*op, types, planes, tile, tile,
                                                                             ts::WorkingSetCall::Apply);
                            const int64_t declared = ts::declaredWorkingSet(*op, tile, tile);
                            EXPECT_LE(peak, declared)
                                << mask.name << (interior ? ", kept inside" : "") << ", tile " << tile << ", "
                                << intensityPlanes << " measured planes, "
                                << (connectivity == Connectivity::EightConnected ? "8" : "4") << "-connected, "
                                << (extractHoles ? "holes kept" : "holes removed") << ": measured " << peak
                                << ", declared " << declared;
                        }
                    }
                }
            }
        }
    }
}

TEST(WorkingSetSegment, CatalogMirrorsConnectedComponents) {
    const OperationPtr op = makeConnectedComponents(ConnectedComponentsParameters());
    const OperationCatalog& catalog = OperationCatalog::instance();
    EXPECT_EQ(catalog.workingSetBytesPerLevelPixel("segment.connected_components"),
              op->workingSetBytesPerLevelPixel());
    EXPECT_EQ(catalog.workingSetBytesPerTile("segment.connected_components"), op->workingSetBytesPerTile());
}

// One small object finished whole, with three measured planes and every
// measurement in micrometres too: what does not scale with the tile, two
// records and their measurement names, is within the per-call figure alone.
TEST(WorkingSetSegment, ConnectedComponentsPerCallCoversOneSmallObject) {
    const Mask square{"square", [](int64_t x, int64_t y) { return x >= 3 && x < 5 && y >= 3 && y < 5; }};
    for (const Connectivity connectivity : {Connectivity::FourConnected, Connectivity::EightConnected}) {
        const OperationPtr op = connectedComponents(connectivity, true);
        const std::vector<DataType> types{DataType::Byte, DataType::Float64, DataType::Float64, DataType::Float64};
        const std::vector<std::vector<uint8_t>> planes = planesOf(square, 8, 8, 3, false);
        const int64_t peak = ts::measureWorkingSetPeakOn(*op, types, planes, 6, 6, ts::WorkingSetCall::Apply);
        EXPECT_LE(peak, op->workingSetBytesPerTile()) << "measured " << peak;
    }
}

namespace {

// Label rasters that maximise what contour tracing allocates: the most
// regions and vertices (four values meeting at every pixel corner, or every
// pixel its own value), the most rings in one region (a region with a hole,
// or an island of another value, at every other pixel), the most vertices per
// region (stripes, diagonals, a staircase) and the trivial ones.
struct LabelRaster {
    const char* name;
    std::function<int32_t(int64_t x, int64_t y)> value;
};

const std::vector<LabelRaster>& adversarialLabelRasters() {
    static const std::vector<LabelRaster> rasters = {
        {"checkerboard of four values",
         [](int64_t x, int64_t y) { return static_cast<int32_t>(1 + x % 2 + 2 * (y % 2)); }},
        {"every pixel its own value", [](int64_t x, int64_t y) { return static_cast<int32_t>(1 + x + 1000 * y); }},
        {"checkerboard of two values", [](int64_t x, int64_t y) { return static_cast<int32_t>(1 + (x + y) % 2); }},
        {"checkerboard with zero", [](int64_t x, int64_t y) { return (x + y) % 2 == 0 ? 1 : 0; }},
        {"diagonals of four values", [](int64_t x, int64_t y) { return static_cast<int32_t>(1 + (x + y) % 4); }},
        {"one-pixel islands", [](int64_t x, int64_t y) {
             return x % 2 == 1 && y % 2 == 1 ? static_cast<int32_t>(2 + (x + 3 * y) % 5) : 1;
         }},
        {"one-pixel holes", [](int64_t x, int64_t y) { return x % 2 == 1 && y % 2 == 1 ? 0 : 1; }},
        {"rows of three values", [](int64_t, int64_t y) { return static_cast<int32_t>(1 + y % 3); }},
        {"columns of three values", [](int64_t x, int64_t) { return static_cast<int32_t>(1 + x % 3); }},
        {"comb", [](int64_t x, int64_t y) { return y % 2 == 0 || x == 1 ? 1 : 2; }},
        {"staircase", [](int64_t x, int64_t y) { return static_cast<int32_t>(1 + (x / 2 + y) % 3); }},
        {"solid", [](int64_t, int64_t) { return 7; }},
        {"empty", [](int64_t, int64_t) { return 0; }},
    };
    return rasters;
}

// One Int32 plane of @p raster; with @p interior, 0 within 3 pixels of the
// edge, so every region is a finished object rather than a seam fragment.
std::vector<std::vector<uint8_t>> labelPlaneOf(const LabelRaster& raster, int64_t width, int64_t height,
                                               bool interior) {
    std::vector<uint8_t> plane(static_cast<size_t>(width * height) * sizeof(int32_t));
    for (int64_t y = 0; y < height; ++y) {
        for (int64_t x = 0; x < width; ++x) {
            const bool inside = !interior || (x >= 3 && y >= 3 && x < width - 3 && y < height - 3);
            const int32_t v = inside ? raster.value(x, y) : 0;
            std::memcpy(&plane[static_cast<size_t>(y * width + x) * sizeof(int32_t)], &v, sizeof(int32_t));
        }
    }
    return {plane};
}

} // namespace

TEST(WorkingSetSegment, ContoursBoundedOnThePattern) {
    for (const bool includeHoles : {true, false}) {
        const auto make = [includeHoles] { return makeContourExtraction(ContourParameters().includeHoles(includeHoles)); };
        for (const DataType type : {DataType::Byte, DataType::Int8, DataType::UInt16, DataType::Int16, DataType::UInt32,
                                    DataType::Int32, DataType::Float32}) {
            expectApplyBounded("segment.contours", make, {type});
        }
    }
}

// The declaration covers the worst label rasters, each region emitted as a seam
// fragment (the rasters reach the tile's edge) or as a finished object (the
// rasters kept inside).
TEST(WorkingSetSegment, ContoursBoundedOnAdversarialLabelRasters) {
    for (const int64_t tile : ts::kWorkingSetTileSizes) {
        const int64_t haloSide = tile;
        for (const LabelRaster& raster : adversarialLabelRasters()) {
            for (const bool interior : {false, true}) {
                const std::vector<std::vector<uint8_t>> planes = labelPlaneOf(raster, haloSide, haloSide, interior);
                for (const bool includeHoles : {true, false}) {
                    const OperationPtr op = makeContourExtraction(ContourParameters().includeHoles(includeHoles));
                    const int64_t peak = ts::measureWorkingSetPeakOn(*op, {DataType::Int32}, planes, tile, tile,
                                                                     ts::WorkingSetCall::Apply);
                    const int64_t declared = ts::declaredWorkingSet(*op, tile, tile);
                    EXPECT_LE(peak, declared) << raster.name << (interior ? ", kept inside" : "") << ", tile "
                                              << tile << ", " << (includeHoles ? "holes kept" : "holes removed")
                                              << ": measured " << peak << ", declared " << declared;
                }
            }
        }
    }
}

TEST(WorkingSetSegment, CatalogMirrorsContours) {
    const OperationPtr op = makeContourExtraction(ContourParameters());
    const OperationCatalog& catalog = OperationCatalog::instance();
    EXPECT_EQ(catalog.workingSetBytesPerLevelPixel("segment.contours"), op->workingSetBytesPerLevelPixel());
    EXPECT_EQ(catalog.workingSetBytesPerTile("segment.contours"), op->workingSetBytesPerTile());
}

// One small object finished whole, its measurements in micrometres too: what
// does not scale with the tile is within the per-call figure alone.
TEST(WorkingSetSegment, ContoursPerCallCoversOneSmallObject) {
    const LabelRaster square{"square", [](int64_t x, int64_t y) { return x >= 3 && x < 5 && y >= 3 && y < 5 ? 3 : 0; }};
    const OperationPtr op = makeContourExtraction(ContourParameters());
    const int64_t peak = ts::measureWorkingSetPeakOn(*op, {DataType::Int32}, labelPlaneOf(square, 8, 8, false), 6, 6,
                                                     ts::WorkingSetCall::Apply);
    EXPECT_LE(peak, op->workingSetBytesPerTile()) << "measured " << peak;
}

namespace {

// Plane 0 of @p value as Float64, then @p intensityPlanes Float64 planes; 0
// within @p margin pixels of the edge.
std::vector<std::vector<uint8_t>> valuePlanesOf(const std::function<double(int64_t x, int64_t y)>& value,
                                                int64_t width, int64_t height, int32_t intensityPlanes,
                                                int64_t margin) {
    std::vector<std::vector<uint8_t>> planes;
    std::vector<uint8_t> plane0(static_cast<size_t>(width * height) * sizeof(double));
    for (int64_t y = 0; y < height; ++y) {
        for (int64_t x = 0; x < width; ++x) {
            const bool inside = x >= margin && y >= margin && x < width - margin && y < height - margin;
            const double v = inside ? value(x, y) : 0.0;
            std::memcpy(&plane0[static_cast<size_t>(y * width + x) * sizeof(double)], &v, sizeof(double));
        }
    }
    planes.push_back(std::move(plane0));
    for (int32_t p = 0; p < intensityPlanes; ++p) {
        std::vector<uint8_t> plane(static_cast<size_t>(width * height) * sizeof(double));
        for (int64_t i = 0; i < width * height; ++i) {
            const double v = static_cast<double>((i * 7 + p) % 101);
            std::memcpy(&plane[static_cast<size_t>(i) * sizeof(double)], &v, sizeof(double));
        }
        planes.push_back(std::move(plane));
    }
    return planes;
}

// An Int64 anchor raster bound as "anchors": @p value, -1 for none.
ts::WorkingSetAuxiliary anchorsOf(const std::function<int64_t(int64_t x, int64_t y)>& value, int64_t width,
                                  int64_t height) {
    ts::WorkingSetAuxiliary anchors;
    anchors.name = "anchors";
    anchors.type = DataType::Int64;
    anchors.bytes.resize(static_cast<size_t>(width * height) * sizeof(int64_t));
    for (int64_t y = 0; y < height; ++y) {
        for (int64_t x = 0; x < width; ++x) {
            const int64_t v = value(x, y);
            std::memcpy(&anchors.bytes[static_cast<size_t>(y * width + x) * sizeof(int64_t)], &v, sizeof(int64_t));
        }
    }
    return anchors;
}

// A spacing of 2 keeps the halo small beside the tile, so what scales with
// the tile weighs most against the declaration.
OperationPtr watershed(WatershedMarkers markers) {
    return makeWatershed(WatershedParameters()
                             .markers(markers)
                             .auxiliaryName("anchors")
                             .minimumMarkerDistancePx(1)
                             .maximumMarkerSpacingPx(2));
}

} // namespace

// The shared pattern's ramps run further than any spacing from their maxima,
// so the operation refuses it; instead, every type, and four planes of mixed
// types, hold cones of height 5 around peaks 8 pixels apart, at the default
// parameters.
TEST(WorkingSetSegment, WatershedBoundedOnCones) {
    const auto cone = [](int64_t x, int64_t y) {
        const int64_t d = std::max(std::abs((x + 4) % 8 - 4), std::abs((y + 4) % 8 - 4));
        return int64_t{5} - d;
    };
    const std::vector<std::vector<DataType>> typeSets = {
        {DataType::Float32, DataType::Float64, DataType::Byte, DataType::UInt16}, {DataType::Byte},
        {DataType::Int8}, {DataType::UInt16}, {DataType::Int16}, {DataType::UInt32}, {DataType::Int32},
        {DataType::Float16}, {DataType::Float32}, {DataType::Float64}, {DataType::Int64}, {DataType::UInt64}};
    for (const int64_t tile : ts::kWorkingSetTileSizes) {
        for (const std::vector<DataType>& types : typeSets) {
            const OperationPtr op = makeWatershed(WatershedParameters());
            const int64_t side = tile + op->halo().left + op->halo().right;
            std::vector<std::vector<uint8_t>> planes;
            for (const DataType type : types) {
                const int64_t size = dataTypeSizeBytes(type);
                std::vector<uint8_t> plane(static_cast<size_t>(side * side * size));
                for (int64_t y = 0; y < side; ++y) {
                    for (int64_t x = 0; x < side; ++x) {
                        bool overflowed = false;
                        detail::writeSample(&plane[static_cast<size_t>(y * side * size)], x, type,
                                            detail::Sample{detail::Sample::Kind::Signed, cone(x, y), 0, 0.0},
                                            RoundingMode::Nearest, OverflowMode::Saturate, overflowed);
                    }
                }
                planes.push_back(std::move(plane));
            }
            const int64_t peak = ts::measureWorkingSetPeakOn(*op, types, planes, tile, tile, ts::WorkingSetCall::Apply);
            EXPECT_LE(peak, ts::declaredWorkingSet(*op, tile, tile))
                << dataTypeName(types.front()) << " and " << types.size() - 1 << " more, tile " << tile
                << ": measured " << peak << ", declared " << ts::declaredWorkingSet(*op, tile, tile);
        }
    }
}

// The declaration covers the worst inputs with up to three measured planes,
// each basin emitted as seam fragments (the input reaches the tile's edge) or
// as finished objects (kept inside): with LocalMaxima markers, a maximum at
// every other pixel of every other row, so that many basins of four pixels
// flood; with AuxiliaryRaster markers, the label rasters of segment.contours
// as anchors, every pixel its own basin at worst, and sparse anchors, so that
// most pixels wait in the queue.
TEST(WorkingSetSegment, WatershedBoundedOnAdversarialInputs) {
    struct Input {
        const char* name;
        WatershedMarkers markers;
        std::function<double(int64_t, int64_t)> value;
        std::function<int64_t(int64_t, int64_t)> anchor;
    };
    const auto none = [](int64_t, int64_t) { return int64_t{-1}; };
    std::vector<Input> inputs = {
        {"maxima on a two-pixel grid", WatershedMarkers::LocalMaxima,
         [](int64_t x, int64_t y) { return x % 2 == 0 && y % 2 == 0 ? 2.0 : 1.0; }, none},
        {"maxima on a two-pixel grid, holes between", WatershedMarkers::LocalMaxima,
         [](int64_t x, int64_t y) {
             return x % 2 == 1 && y % 2 == 1 ? 0.0 : (x % 2 == 0 && y % 2 == 0 ? 2.0 : 1.0);
         },
         none},
        // Falling away from each anchor, so each pixel floods from a near one.
        {"sparse anchors", WatershedMarkers::AuxiliaryRaster,
         [](int64_t x, int64_t y) {
             return 4.0 - static_cast<double>(std::min(x % 3, 3 - x % 3) + std::min(y % 3, 3 - y % 3));
         },
         [](int64_t x, int64_t y) { return x % 3 == 0 && y % 3 == 0 ? x + 1000 * y : int64_t{-1}; }},
    };
    for (const LabelRaster& raster : adversarialLabelRasters()) {
        // The raster's 0 is background in plane 0 too, so every foreground pixel
        // is a marker.
        inputs.push_back({raster.name, WatershedMarkers::AuxiliaryRaster,
                          [&raster](int64_t x, int64_t y) { return raster.value(x, y) == 0 ? 0.0 : 1.0; },
                          [&raster](int64_t x, int64_t y) {
                              const int32_t v = raster.value(x, y);
                              return v == 0 ? int64_t{-1} : int64_t{v};
                          }});
    }
    for (const int64_t tile : ts::kWorkingSetTileSizes) {
        for (const Input& input : inputs) {
            const OperationPtr probe = watershed(input.markers);
            const int64_t haloSide = tile + probe->halo().left + probe->halo().right;
            const ts::WorkingSetAuxiliary anchors = anchorsOf(input.anchor, haloSide, haloSide);
            for (const bool interior : {false, true}) {
                const int64_t margin = interior ? probe->halo().left + 3 : 0;
                for (const int32_t intensityPlanes : {0, 1, 3}) {
                    const std::vector<DataType> types(1 + static_cast<size_t>(intensityPlanes), DataType::Float64);
                    const std::vector<std::vector<uint8_t>> planes =
                        valuePlanesOf(input.value, haloSide, haloSide, intensityPlanes, margin);
                    const OperationPtr op = watershed(input.markers);
                    int64_t peak = 0;
                    try {
                        peak = ts::measureWorkingSetPeakOn(*op, types, planes, tile, tile, ts::WorkingSetCall::Apply,
                                                           &anchors);
                    } catch (const std::exception& e) {
                        ADD_FAILURE() << input.name << ", tile " << tile << ": " << e.what();
                        continue;
                    }
                    const int64_t declared = ts::declaredWorkingSet(*op, tile, tile);
                    EXPECT_LE(peak, declared) << input.name << (interior ? ", kept inside" : "") << ", tile "
                                              << tile << ", " << intensityPlanes << " measured planes: measured "
                                              << peak << ", declared " << declared;
                }
            }
        }
    }
}

TEST(WorkingSetSegment, CatalogMirrorsWatershed) {
    const OperationPtr op = makeWatershed(WatershedParameters());
    const OperationCatalog& catalog = OperationCatalog::instance();
    EXPECT_EQ(catalog.workingSetBytesPerLevelPixel("segment.watershed"), op->workingSetBytesPerLevelPixel());
    EXPECT_EQ(catalog.workingSetBytesPerTile("segment.watershed"), op->workingSetBytesPerTile());
}

// One small object finished whole, with three measured planes and every
// measurement in micrometres too: what does not scale with the tile is within
// the per-call figure alone.
TEST(WorkingSetSegment, WatershedPerCallCoversOneSmallObject) {
    const OperationPtr op =
        makeWatershed(WatershedParameters().minimumMarkerDistancePx(1).maximumMarkerSpacingPx(1));
    const std::vector<DataType> types{DataType::Float64, DataType::Float64, DataType::Float64, DataType::Float64};
    // The halo'd tile is 12 x 12 pixels: a 2 x 2 square inside the 6 x 6 tile.
    const std::vector<std::vector<uint8_t>> planes = valuePlanesOf(
        [](int64_t x, int64_t y) { return x >= 5 && x < 7 && y >= 5 && y < 7 ? 1.0 : 0.0; }, 12, 12, 3, 0);
    const int64_t peak = ts::measureWorkingSetPeakOn(*op, types, planes, 6, 6, ts::WorkingSetCall::Apply);
    EXPECT_LE(peak, op->workingSetBytesPerTile()) << "measured " << peak;
}

namespace {

// Three Byte planes, pink where @p mask is foreground and white elsewhere; with
// @p margin, white within that many pixels of the edge.
std::vector<std::vector<uint8_t>> rgbPlanesOf(const Mask& mask, int64_t width, int64_t height, int64_t margin) {
    static const uint8_t pink[3] = {225, 150, 195};
    std::vector<std::vector<uint8_t>> planes(3, std::vector<uint8_t>(static_cast<size_t>(width * height)));
    for (int64_t y = 0; y < height; ++y) {
        for (int64_t x = 0; x < width; ++x) {
            const bool inside = x >= margin && y >= margin && x < width - margin && y < height - margin;
            const bool tissue = inside && mask.foreground(x, y);
            for (size_t p = 0; p < 3; ++p) {
                planes[p][static_cast<size_t>(y * width + x)] = tissue ? pink[p] : 245;
            }
        }
    }
    return planes;
}

OperationPtr tissueDetect(int32_t radius) {
    return makeTissueDetect(TissueDetectParameters().closingRadiusPx(radius));
}

} // namespace

TEST(WorkingSetSegment, TissueDetectBoundedOnThePattern) {
    for (const int32_t radius : {0, 2}) {
        const auto make = [radius] { return tissueDetect(radius); };
        expectApplyBounded("segment.tissue_detect", make, {DataType::Byte, DataType::Byte, DataType::Byte});
        expectApplyBounded("segment.tissue_detect", make, {DataType::UInt16, DataType::UInt16, DataType::UInt16});
    }
}

// The declaration covers the worst masks, each region emitted as a seam
// fragment (the masks reach the tile's edge) or as a finished object (the
// masks kept inside): without a closing the masks are traced as they are; a
// closing of radius 1 or 2 fills their gaps and holes first.
TEST(WorkingSetSegment, TissueDetectBoundedOnAdversarialMasks) {
    for (const int64_t tile : ts::kWorkingSetTileSizes) {
        for (const int32_t radius : {0, 1, 2}) {
            const int64_t halo = 2 * radius + 1;
            const int64_t haloSide = tile + 2 * halo;
            for (const Mask& mask : adversarialMasks()) {
                for (const bool interior : {false, true}) {
                    const std::vector<std::vector<uint8_t>> planes =
                        rgbPlanesOf(mask, haloSide, haloSide, interior ? halo + 3 : 0);
                    const OperationPtr op = tissueDetect(radius);
                    const int64_t peak =
                        ts::measureWorkingSetPeakOn(*op, {DataType::Byte, DataType::Byte, DataType::Byte}, planes,
                                                    tile, tile, ts::WorkingSetCall::Apply);
                    const int64_t declared = ts::declaredWorkingSet(*op, tile, tile);
                    EXPECT_LE(peak, declared) << mask.name << (interior ? ", kept inside" : "") << ", tile " << tile
                                              << ", radius " << radius << ": measured " << peak << ", declared "
                                              << declared;
                }
            }
        }
    }
}

TEST(WorkingSetSegment, CatalogMirrorsTissueDetect) {
    const OperationPtr op = makeTissueDetect(TissueDetectParameters());
    const OperationCatalog& catalog = OperationCatalog::instance();
    EXPECT_EQ(catalog.workingSetBytesPerLevelPixel("segment.tissue_detect"), op->workingSetBytesPerLevelPixel());
    EXPECT_EQ(catalog.workingSetBytesPerTile("segment.tissue_detect"), op->workingSetBytesPerTile());
}

// One small object finished whole, its measurements in micrometres too: what
// does not scale with the tile is within the per-call figure alone.
TEST(WorkingSetSegment, TissueDetectPerCallCoversOneSmallObject) {
    const Mask square{"square", [](int64_t x, int64_t y) { return x >= 3 && x < 5 && y >= 3 && y < 5; }};
    const OperationPtr op = tissueDetect(0);
    const int64_t peak =
        ts::measureWorkingSetPeakOn(*op, {DataType::Byte, DataType::Byte, DataType::Byte},
                                    rgbPlanesOf(square, 8, 8, 0), 6, 6, ts::WorkingSetCall::Apply);
    EXPECT_LE(peak, op->workingSetBytesPerTile()) << "measured " << peak;
}
