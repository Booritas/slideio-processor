include(GoogleTest)

# Shared test helpers (synthetic scenes, test operations) that plans B and C
# build on. The synthetic scene and the test operations use only the public
# headers; operation_conformance.cpp also compares parameters through the
# engine's detail header, and test_operations.cpp normalises rings through the
# engine's fragment_merge.hpp, so src/ is on the library's private include path.
# Neither is exported from the engine, so their sources are compiled into the
# library, and every executable that links the library gets them from there.
add_library(slideio_processor_pipeline_test_support STATIC
    test_support/synthetic_scene.cpp
    test_support/operation_conformance.cpp
    test_support/test_operations.cpp
    test_support/executor_support.cpp
    test_support/disc_mask_source.cpp
    test_support/global_ops.cpp
    test_support/blob_scene.cpp
    ${PROJECT_SOURCE_DIR}/src/pipeline/detail/fragment_merge.cpp
    ${PROJECT_SOURCE_DIR}/src/pipeline/detail/parameters_json.cpp
)
target_link_libraries(slideio_processor_pipeline_test_support PUBLIC slideio-processor-engine GTest::gtest
    PRIVATE rapidjson)
target_include_directories(slideio_processor_pipeline_test_support PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_include_directories(slideio_processor_pipeline_test_support PUBLIC ${PROJECT_SOURCE_DIR}/tests)

# A third-party operation compiled against the public headers alone (UR-EXT-2):
# its include path is include/ and SlideIO's, with no src/ and no OpenCV, and it
# links nothing, so it cannot reach an internal symbol either.
add_library(pipeline_public_only OBJECT
    public_only/third_party_operation.cpp
)
target_include_directories(pipeline_public_only PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    $<TARGET_PROPERTY:slideio::slideio,INTERFACE_INCLUDE_DIRECTORIES>)

add_executable(slideio_processor_pipeline_tests
    test_backend.cpp
    test_bindings.cpp
    test_executor.cpp
    test_fragment_merge.cpp
    test_global_context.cpp
    test_lazy_scene.cpp
    test_object_finishing.cpp
    test_pixel_size.cpp
    test_operation.cpp
    test_operation_catalog.cpp
    test_operation_parameters.cpp
    test_pipeline_spec.cpp
    test_pipeline_validate.cpp
    test_restriction.cpp
    test_reduction.cpp
    test_scene_reader.cpp
    test_seam_merger.cpp
    test_seam_run.cpp
    test_seam_spill.cpp
    test_seg3_tiling_invariance.cpp
    test_tile_evaluator.cpp
    test_tile_grid.cpp
    test_umbrella.cpp
    test_working_set.cpp
    $<TARGET_OBJECTS:pipeline_public_only>
    # Not exported from the library, so compiled into the test directly
    # (fragment_merge.cpp and parameters_json.cpp come with the test-support library).
    ${PROJECT_SOURCE_DIR}/src/pipeline/detail/combine_partials.cpp
    ${PROJECT_SOURCE_DIR}/src/pipeline/detail/finishing_sink.cpp
    ${PROJECT_SOURCE_DIR}/src/pipeline/detail/half.cpp
    ${PROJECT_SOURCE_DIR}/src/pipeline/detail/level_table.cpp
    ${PROJECT_SOURCE_DIR}/src/pipeline/detail/operation_context_state.cpp
    ${PROJECT_SOURCE_DIR}/src/pipeline/detail/pixel_size.cpp
    ${PROJECT_SOURCE_DIR}/src/pipeline/detail/plane_buffers.cpp
    ${PROJECT_SOURCE_DIR}/src/pipeline/detail/restriction.cpp
    ${PROJECT_SOURCE_DIR}/src/pipeline/detail/scene_reader.cpp
    ${PROJECT_SOURCE_DIR}/src/pipeline/detail/tile_evaluator.cpp
    ${PROJECT_SOURCE_DIR}/src/pipeline/detail/type_contract.cpp
)
target_link_libraries(slideio_processor_pipeline_tests PRIVATE
    slideio-processor-engine slideio_processor_pipeline_test_support rapidjson GTest::gtest_main)
target_include_directories(slideio_processor_pipeline_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/tests)

# The tests load SlideIO's DLLs, which live in SlideIO's install tree rather than
# beside the test. On Windows each test gets that bin directory prepended
# to PATH; on Linux and macOS the imported library's RPATH suffices. PRE_TEST
# discovery means the build never runs the executable. Discovery at ctest time does
# run it, outside the tests' ENVIRONMENT_MODIFICATION, so the same PATH also goes
# through TEST_LAUNCHER (CMake 3.29+), which wraps discovery and every run alike.
set(PIPELINE_TEST_PROPERTIES TIMEOUT 60)
if(WIN32)
    if(CMAKE_VERSION VERSION_LESS 3.29)
        # Before 3.29 TEST_LAUNCHER is ignored, so test discovery runs without
        # SlideIO's DLLs on PATH and the pipeline tests cannot be listed or run
        # (before 3.22 ENVIRONMENT_MODIFICATION is ignored as well).
        message(WARNING "tests/pipeline: CMake ${CMAKE_VERSION} ignores TEST_LAUNCHER; the pipeline tests "
                        "need CMake 3.29 or later on Windows to find SlideIO's DLLs.")
    endif()
    get_target_property(SLIDEIO_RUNTIME slideio::slideio IMPORTED_LOCATION_RELEASE)
    if(NOT SLIDEIO_RUNTIME)
        # SlideIO's hand-written package config sets the configuration-less property.
        get_target_property(SLIDEIO_RUNTIME slideio::slideio IMPORTED_LOCATION)
    endif()
    if(NOT SLIDEIO_RUNTIME)
        message(FATAL_ERROR "tests/pipeline: slideio::slideio has no imported location")
    endif()
    get_filename_component(SLIDEIO_BIN_DIR "${SLIDEIO_RUNTIME}" DIRECTORY)
    file(TO_NATIVE_PATH "${SLIDEIO_BIN_DIR}" SLIDEIO_BIN_DIR)
    list(APPEND PIPELINE_TEST_PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${SLIDEIO_BIN_DIR}")
    set_property(TARGET slideio_processor_pipeline_tests PROPERTY TEST_LAUNCHER
        ${CMAKE_COMMAND} -E env --modify "PATH=path_list_prepend:${SLIDEIO_BIN_DIR}" --)
endif()
# A hung waiter fails after 60 s instead of blocking the run.
gtest_discover_tests(slideio_processor_pipeline_tests DISCOVERY_TIMEOUT 60 TEST_PREFIX "pipeline."
    DISCOVERY_MODE PRE_TEST
    PROPERTIES ${PIPELINE_TEST_PROPERTIES})

# Every public header must compile on its own, with the SlideIO and data include
# paths but no OpenCV include path: a header that reaches OpenCV fails
# the normal build. One source per public header, of pipeline and algorithm.
add_library(pipeline_headers_are_opencv_free OBJECT
    header_check/check_algorithm_measure_measure.cpp
    header_check/check_algorithm_pixel_colour.cpp
    header_check/check_algorithm_pixel_convert.cpp
    header_check/check_algorithm_pixel_filter.cpp
    header_check/check_algorithm_pixel_threshold.cpp
    header_check/check_algorithm_registry.cpp
    header_check/check_algorithm_segment_segment.cpp
    header_check/check_backend.cpp
    header_check/check_executor.cpp
    header_check/check_export.cpp
    header_check/check_operation.cpp
    header_check/check_operation_catalog.cpp
    header_check/check_operation_parameters.cpp
    header_check/check_pipeline.cpp
    header_check/check_pipeline_all.cpp
    header_check/check_pipeline_spec.cpp
    header_check/check_reduction.cpp
    header_check/check_seam_merger.cpp
    header_check/check_standard_measurements.cpp
    header_check/check_tile_grid.cpp
)
# A new algorithm header without its check source would escape the check, so
# configuring fails until header_check/check_algorithm_<path>.cpp exists for
# every include/slideio/processor/algorithm/<path>.hpp ('/' becomes '_').
file(GLOB_RECURSE ALGORITHM_PUBLIC_HEADERS RELATIVE "${PROJECT_SOURCE_DIR}/include/slideio/processor/algorithm"
    CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/include/slideio/processor/algorithm/*.hpp")
get_target_property(HEADER_CHECK_SOURCES pipeline_headers_are_opencv_free SOURCES)
foreach(header IN LISTS ALGORITHM_PUBLIC_HEADERS)
    string(REGEX REPLACE "\\.hpp$" "" stem "${header}")
    string(REPLACE "/" "_" stem "${stem}")
    list(FIND HEADER_CHECK_SOURCES "header_check/check_algorithm_${stem}.cpp" listed)
    if(listed EQUAL -1 OR NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/header_check/check_algorithm_${stem}.cpp")
        message(FATAL_ERROR "tests/pipeline: include/slideio/processor/algorithm/${header} has no header check; "
            "add header_check/check_algorithm_${stem}.cpp and list it in pipeline_headers_are_opencv_free")
    endif()
endforeach()
target_include_directories(pipeline_headers_are_opencv_free PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    $<TARGET_PROPERTY:slideio::slideio,INTERFACE_INCLUDE_DIRECTORIES>)
