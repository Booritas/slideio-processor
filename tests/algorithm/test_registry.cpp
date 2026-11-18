// Requirements: UR-EXT-5, UR-IMG-12, UR-ALG-4, UR-ALG-6, UR-ALG-8, UR-STG-4
#include <slideio/processor/algorithm/registry.hpp>
#include <slideio/processor/config/configuration.hpp>
#include <slideio/processor/core/data_type.hpp>
#include <slideio/processor/core/error.hpp>
#include <slideio/processor/pipeline/backend.hpp>
#include <slideio/processor/pipeline/operation_catalog.hpp>
#include <slideio/processor/pipeline/pipeline.hpp>

#include <pipeline/test_support/operation_conformance.hpp>
#include <pipeline/test_support/synthetic_scene.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace slideio::processor;
namespace ts = slideio::processor::testing_support;

namespace {

bool isBuiltinName(const char* name) {
    return std::strncmp(name, "pixel.", 6) == 0 || std::strncmp(name, "segment.", 8) == 0 ||
           std::strncmp(name, "measure.", 8) == 0;
}

std::vector<std::string> builtinNames() {
    std::vector<std::string> names;
    const OperationCatalog& catalog = OperationCatalog::instance();
    for (int32_t i = 0; i < catalog.count(); ++i) {
        if (isBuiltinName(catalog.nameAt(i))) {
            names.emplace_back(catalog.nameAt(i));
        }
    }
    return names;
}

std::vector<std::string> splitCsv(const std::string& line) {
    std::vector<std::string> cells;
    std::stringstream stream(line);
    std::string cell;
    while (std::getline(stream, cell, ',')) {
        cells.push_back(cell);
    }
    return cells;
}

// Whether a one-operation pipeline over a 1-channel synthetic scene of @p type validates.
bool pipelineValidates(const char* name, DataType type) {
    const auto slideioType = static_cast<slideio::DataType>(dataTypeToSlideio(type));
    auto scene = ts::makeSyntheticScene({{16, 16, 0, 0}}, {slideioType}, [](int, int64_t, int64_t) { return 0.0; });
    PipelineSpec spec;
    spec.operation(name, OperationParameters());
    const Configuration config = ConfigurationBuilder().tileSize(16, 16).tileSizeFromSlide(false).workerCount(1).build();
    try {
        Pipeline(scene, config, spec).validate();
        return true;
    } catch (const ProcessorError&) {
        return false;
    }
}

} // namespace

TEST(Registry, RegisteredAtLoad) {
    for (const char* name : {"pixel.threshold_fixed", "pixel.convert_type", "pixel.colour_convert",
                             "pixel.stain_deconvolution", "pixel.threshold_otsu", "pixel.threshold_adaptive"}) {
        EXPECT_TRUE(OperationCatalog::instance().isRegistered(name)) << name;
    }
}

TEST(Registry, IdempotentAndThreadSafe) {
    const int32_t before = OperationCatalog::instance().count();
    std::atomic<int32_t> failures{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&failures] {
            for (int i = 0; i < 100; ++i) {
                try {
                    registerBuiltinOperations();
                    registerPixelOperations();
                    registerSegmentationOperations();
                    registerMeasurementOperations();
                } catch (...) {
                    ++failures;
                }
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    EXPECT_EQ(failures.load(), 0);
    EXPECT_EQ(OperationCatalog::instance().count(), before);
}

TEST(Registry, EveryBuiltinIsConformant) {
    const std::vector<std::string> names = builtinNames();
    ASSERT_FALSE(names.empty());
    const OperationCatalog& catalog = OperationCatalog::instance();
    for (const std::string& name : names) {
        ts::expectConformant(name.c_str());
        EXPECT_GT(std::strlen(catalog.correctnessReference(name.c_str())), 0u) << name;
        EXPECT_GT(std::strlen(catalog.complexityNote(name.c_str())), 0u) << name;
    }
}

TEST(Registry, SupportMatrixRowsMatchRuntime) {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        ("slideio_processor_algorithm_matrix_" + std::to_string(std::hash<std::thread::id>()(std::this_thread::get_id())) +
         ".csv");
    const OperationCatalog& catalog = OperationCatalog::instance();
    ASSERT_GT(catalog.writeSupportMatrix(path.u8string().c_str()), 0);

    std::vector<std::string> lines;
    {
        std::ifstream in(path, std::ios::binary);
        std::string line;
        while (std::getline(in, line)) {
            lines.push_back(line);
        }
    }
    std::filesystem::remove(path);
    ASSERT_FALSE(lines.empty());
    const std::vector<std::string> header = splitCsv(lines.front());
    ASSERT_EQ(header.size(), 12u);

    int32_t rowsChecked = 0;
    for (size_t r = 1; r < lines.size(); ++r) {
        const std::vector<std::string> cells = splitCsv(lines[r]);
        ASSERT_EQ(cells.size(), 12u) << lines[r];
        const std::string& name = cells.front();
        if (!isBuiltinName(name.c_str())) {
            continue;
        }
        ++rowsChecked;
        for (int32_t i = 0; i < 11; ++i) {
            const auto type = static_cast<DataType>(i);
            ASSERT_EQ(header[static_cast<size_t>(i) + 1], dataTypeName(type));
            const std::string& cell = cells[static_cast<size_t>(i) + 1];
            ASSERT_TRUE(cell == "yes" || cell == "no") << name << " " << dataTypeName(type);
            // The synthetic scene cannot produce Float16; that type is checked against the declarations.
            const bool runs = type == DataType::Float16
                                  ? catalog.supportedTypes(name.c_str()).contains(type) && Backend::instance().supports(type)
                                  : pipelineValidates(name.c_str(), type);
            EXPECT_EQ(cell == "yes", runs) << name << " " << dataTypeName(type);
        }
    }
    EXPECT_EQ(rowsChecked, static_cast<int32_t>(builtinNames().size()));
}
