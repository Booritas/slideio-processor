// Requirements: UR-EXE-2, UR-ALG-8, UR-SEG-2
#include <slideio/processor/algorithm/segment/segment.hpp>
#include <slideio/processor/pipeline/operation_catalog.hpp>
#include <slideio/processor/pipeline/operation_parameters.hpp>

#include <algorithm/support/working_set_check.hpp>

#include <gtest/gtest.h>

#include <cstdint>
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

// Plane 0 as the mask, then @p intensityPlanes Float64 planes.
std::vector<std::vector<uint8_t>> planesOf(const Mask& mask, int64_t width, int64_t height, int32_t intensityPlanes) {
    std::vector<std::vector<uint8_t>> planes;
    std::vector<uint8_t> plane0(static_cast<size_t>(width * height));
    for (int64_t y = 0; y < height; ++y) {
        for (int64_t x = 0; x < width; ++x) {
            plane0[static_cast<size_t>(y * width + x)] = mask.foreground(x, y) ? 1 : 0;
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

// The declaration covers the worst masks with up to three measured planes.
TEST(WorkingSetSegment, ConnectedComponentsBoundedOnAdversarialMasks) {
    for (const int64_t tile : ts::kWorkingSetTileSizes) {
        const int64_t haloSide = tile + 2;
        for (const Mask& mask : adversarialMasks()) {
            for (const int32_t intensityPlanes : {0, 1, 3}) {
                std::vector<DataType> types{DataType::Byte};
                types.insert(types.end(), static_cast<size_t>(intensityPlanes), DataType::Float64);
                const std::vector<std::vector<uint8_t>> planes =
                    planesOf(mask, haloSide, haloSide, intensityPlanes);
                for (const Connectivity connectivity : {Connectivity::FourConnected, Connectivity::EightConnected}) {
                    for (const bool extractHoles : {true, false}) {
                        const OperationPtr op = connectedComponents(connectivity, extractHoles);
                        const int64_t peak =
                            ts::measureWorkingSetPeakOn(*op, types, planes, tile, tile, ts::WorkingSetCall::Apply);
                        const int64_t declared = ts::declaredWorkingSet(*op, tile, tile);
                        EXPECT_LE(peak, declared)
                            << mask.name << ", tile " << tile << ", " << intensityPlanes << " measured planes, "
                            << (connectivity == Connectivity::EightConnected ? "8" : "4") << "-connected, "
                            << (extractHoles ? "holes kept" : "holes removed") << ": measured " << peak
                            << ", declared " << declared;
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
