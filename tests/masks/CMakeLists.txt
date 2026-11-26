include(GoogleTest)

# The masks module's tests (reporting plan C). They link SlideIO, so they are an engine-side
# target; the data library never does. The pipeline test support provides the pipeline
# scenes MaskWriter is tested on. test_ome_layout.cpp writes its raster through
# detail::writePyramidTiff, which the data library does not export, so its sources are
# compiled in here as tests/storage does, with libtiff, zlib and rapidjson linked directly.
add_executable(slideio_processor_masks_tests
    test_ome_layout.cpp
    test_label_scene.cpp
    test_mask_writer.cpp
    ${PROJECT_SOURCE_DIR}/src/storage/detail/file_io.cpp
    ${PROJECT_SOURCE_DIR}/src/storage/detail/half_float.cpp
    ${PROJECT_SOURCE_DIR}/src/storage/detail/pyramid_tiff_writer.cpp
    ${PROJECT_SOURCE_DIR}/src/storage/detail/tiff_io.cpp
    ${PROJECT_SOURCE_DIR}/src/storage/detail/tile_spool.cpp
)
target_link_libraries(slideio_processor_masks_tests PRIVATE
    slideio-processor-engine slideio_processor_pipeline_test_support slideio::slideio slideio-processor-data
    rapidjson TIFF::TIFF ZLIB::ZLIB GTest::gtest_main)
target_include_directories(slideio_processor_masks_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/tests)

# SlideIO's DLLs are found beside the executable (tests/CMakeLists.txt).
slideio_processor_copy_slideio_runtime(slideio_processor_masks_tests)
gtest_discover_tests(slideio_processor_masks_tests DISCOVERY_TIMEOUT 60 TEST_PREFIX "masks."
    PROPERTIES TIMEOUT 60)
