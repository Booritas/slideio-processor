include(GoogleTest)

# The algorithm tests reach the pipeline's detail headers (the operation context,
# parameter comparison) and the algorithm's own sample codec, colour maths and
# OpenCV plane helpers, so src/ and tests/ are on their include path; the
# algorithm library itself never has src/ on its path. Detail code the engine
# does not export is compiled in directly.
add_executable(slideio_processor_algorithm_tests
    test_registry.cpp
    test_colour_convert.cpp
    test_stain_deconvolution.cpp
    test_convert_type.cpp
    test_threshold_fixed.cpp
    test_threshold_otsu.cpp
    test_threshold_adaptive.cpp
    test_gaussian_blur.cpp
    test_derivatives.cpp
    test_median_blur.cpp
    test_morphology.cpp
    test_bilateral_filter.cpp
    test_canny.cpp
    test_cv_plane.cpp
    test_region_trace.cpp
    test_measurement_finish.cpp
    support/harness.cpp
    ${PROJECT_SOURCE_DIR}/src/algorithm/detail/colour_math.cpp
    ${PROJECT_SOURCE_DIR}/src/algorithm/detail/sample_codec.cpp
    ${PROJECT_SOURCE_DIR}/src/algorithm/detail/cv_plane.cpp
    ${PROJECT_SOURCE_DIR}/src/algorithm/detail/measurement_finish.cpp
    ${PROJECT_SOURCE_DIR}/src/algorithm/detail/region_emit.cpp
    ${PROJECT_SOURCE_DIR}/src/algorithm/detail/region_trace.cpp
    ${PROJECT_SOURCE_DIR}/src/pipeline/detail/operation_context_state.cpp
)
target_link_libraries(slideio_processor_algorithm_tests PRIVATE
    slideio-processor-engine slideio_processor_pipeline_test_support rapidjson GTest::gtest_main)
# The derivative tests compare their coefficients with OpenCV's own kernels and
# filters, so OpenCV's image processing is linked here, for the tests only.
target_link_libraries(slideio_processor_algorithm_tests PRIVATE opencv_core opencv_imgproc)
target_include_directories(slideio_processor_algorithm_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/src/algorithm
    ${PROJECT_SOURCE_DIR}/tests)
# The tests compare floating-point results bit for bit with direct references
# and across tile sizes, so neither side may fuse a multiply and an add into
# an FMA the other does not. MSVC does not contract by default.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(slideio_processor_algorithm_tests PRIVATE -ffp-contract=off)
endif()

# SlideIO's DLLs live in its install tree; see tests/pipeline/CMakeLists.txt for
# why PATH goes through both ENVIRONMENT_MODIFICATION and TEST_LAUNCHER.
set(ALGORITHM_TEST_PROPERTIES TIMEOUT 120)
if(WIN32)
    get_target_property(SLIDEIO_RUNTIME slideio::slideio IMPORTED_LOCATION_RELEASE)
    if(NOT SLIDEIO_RUNTIME)
        get_target_property(SLIDEIO_RUNTIME slideio::slideio IMPORTED_LOCATION)
    endif()
    if(NOT SLIDEIO_RUNTIME)
        message(FATAL_ERROR "tests/algorithm: slideio::slideio has no imported location")
    endif()
    get_filename_component(SLIDEIO_BIN_DIR "${SLIDEIO_RUNTIME}" DIRECTORY)
    file(TO_NATIVE_PATH "${SLIDEIO_BIN_DIR}" SLIDEIO_BIN_DIR)
    list(APPEND ALGORITHM_TEST_PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${SLIDEIO_BIN_DIR}")
    set_property(TARGET slideio_processor_algorithm_tests PROPERTY TEST_LAUNCHER
        ${CMAKE_COMMAND} -E env --modify "PATH=path_list_prepend:${SLIDEIO_BIN_DIR}" --)
endif()
gtest_discover_tests(slideio_processor_algorithm_tests TEST_PREFIX "algorithm."
    DISCOVERY_MODE PRE_TEST
    PROPERTIES ${ALGORITHM_TEST_PROPERTIES})

# Working-set tests measure what an operation allocates through operator new,
# which this executable replaces with a counting pair (support/allocation_meter).
# A shared library may allocate through its own operator new (under MSVC every
# module has its own), so the engine's pipeline and algorithm code is compiled
# into this executable instead of being taken from the engine library; the
# sources are compiled with the engine's export switch, so the engine headers
# declare those symbols as defined here rather than imported. The data library
# and SlideIO are still linked as libraries: they allocate nothing an operation
# measures.
add_executable(slideio_processor_working_set_tests
    test_allocation_meter.cpp
    test_working_set_pixel.cpp
    test_working_set_filter.cpp
    support/allocation_meter.cpp
    support/working_set_check.cpp
    support/harness.cpp
    ${PROJECT_SOURCE_DIR}/tests/pipeline/test_support/synthetic_scene.cpp
    $<TARGET_OBJECTS:slideio-processor-pipeline>
    $<TARGET_OBJECTS:slideio-processor-algorithm>
)
target_compile_definitions(slideio_processor_working_set_tests PRIVATE SLIDEIO_PROCESSOR_ENGINE_API)
target_link_libraries(slideio_processor_working_set_tests PRIVATE
    slideio-processor-data slideio::slideio opencv_core opencv_imgproc rapidjson GTest::gtest_main)
target_include_directories(slideio_processor_working_set_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/src/algorithm
    ${PROJECT_SOURCE_DIR}/tests)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(slideio_processor_working_set_tests PRIVATE -ffp-contract=off)
endif()
# As the engine does: the static OpenCV compiled in here must not interpose on,
# or be interposed by, the copy SlideIO's libraries carry.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_options(slideio_processor_working_set_tests PRIVATE -Wl,--exclude-libs,ALL)
endif()
if(WIN32)
    set_property(TARGET slideio_processor_working_set_tests PROPERTY TEST_LAUNCHER
        ${CMAKE_COMMAND} -E env --modify "PATH=path_list_prepend:${SLIDEIO_BIN_DIR}" --)
endif()
gtest_discover_tests(slideio_processor_working_set_tests TEST_PREFIX "algorithm.ws."
    DISCOVERY_MODE PRE_TEST
    PROPERTIES ${ALGORITHM_TEST_PROPERTIES})
