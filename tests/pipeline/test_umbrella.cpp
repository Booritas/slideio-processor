// Requirements: UR-API-2
#include <slideio/processor/pipeline/pipeline_all.hpp>

#include <gtest/gtest.h>

using namespace slideio::processor;

#ifndef SLIDEIO_PROCESSOR_ENGINE_EXPORTS
#error "export.hpp is not reached through the umbrella header"
#endif

// One name from each public pipeline header, reached through the umbrella alone.
TEST(PipelineUmbrella, ExposesTheWholeModule) {
    EXPECT_STREQ(Backend::instance().name(), "OpenCV");                     // backend.hpp
    (void)sizeof(WorkerPool);                                               // executor.hpp
    (void)sizeof(TileExecutor);                                             // executor.hpp
    EXPECT_TRUE(Halo::none().isEmpty());                                    // operation.hpp
    (void)sizeof(IntensityPartial);                                         // operation.hpp
    (void)&OperationCatalog::instance;                                      // operation_catalog.hpp
    OperationParameters parameters;                                         // operation_parameters.hpp
    parameters.setInt("k", 1);
    EXPECT_EQ(parameters.count(), 1);
    (void)sizeof(Pipeline);                                                 // pipeline.hpp
    PipelineSpec spec;                                                      // pipeline_spec.hpp
    EXPECT_EQ(spec.operationCount(), 0);
    Histogram64 histogram(4, 0.0, 1.0);                                     // reduction.hpp
    EXPECT_EQ(histogram.totalCount(), 0);
    (void)sizeof(SeamMerger);                                               // seam_merger.hpp
    (void)&applyStandardMeasurements;                                       // standard_measurements.hpp
    (void)sizeof(TileGrid);                                                 // tile_grid.hpp
    (void)&defaultTileSize;                                                 // tile_grid.hpp
}
