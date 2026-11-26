include(GoogleTest)

# The workspace tests write their slides with libtiff (support/test_slide.cpp), so
# SlideIO opens a real file through its own driver lookup. src/ is on the include
# path for the create-failure test seam (src/workspace/detail/create_fault.hpp).
add_executable(slideio_processor_workspace_tests
    support/aux_probe.cpp
    support/test_slide.cpp
    # storage/raster_test_util.hpp's makeView() calls detail::floatToHalf. half_float.cpp
    # lives in the slideio-processor-data shared library, which does not export it (no
    # export macro, hidden visibility), so the test compiles its own copy, as the storage
    # tests do; the library's copy stays internal to it and the two never meet.
    ${PROJECT_SOURCE_DIR}/src/storage/detail/half_float.cpp
    test_layer_lifecycle.cpp
    test_coarse_to_fine.cpp
    test_stage_definition.cpp
    test_stage_registry.cpp
    test_staleness_graph.cpp
    test_stage_runner.cpp
    test_workspace.cpp
    test_paths_in_diagnostics.cpp
)
target_link_libraries(slideio_processor_workspace_tests PRIVATE
    slideio-processor-engine slideio_processor_pipeline_test_support SQLite::SQLite3 TIFF::TIFF rapidjson GTest::gtest_main)
# The v1 golden store of the storage tests: a refused scene must leave it unmigrated.
target_compile_definitions(slideio_processor_workspace_tests PRIVATE
    SLIDEIO_PROCESSOR_TEST_DATA_DIR="${PROJECT_SOURCE_DIR}/tests/storage/data")
target_include_directories(slideio_processor_workspace_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/tests)

# SlideIO's DLLs are found beside the executable (tests/CMakeLists.txt).
slideio_processor_copy_slideio_runtime(slideio_processor_workspace_tests)
gtest_discover_tests(slideio_processor_workspace_tests DISCOVERY_TIMEOUT 60 TEST_PREFIX "workspace."
    PROPERTIES TIMEOUT 60)
