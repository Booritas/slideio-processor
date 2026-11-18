include(GoogleTest)

# The algorithm tests reach the pipeline's detail headers (the operation context,
# parameter comparison) and the algorithm's own sample codec, so src/ and tests/
# are on their include path; the algorithm library itself never has src/ on its
# path. Detail code the engine does not export is compiled in directly.
add_executable(slideio_processor_algorithm_tests
    test_registry.cpp
    test_threshold_fixed.cpp
    support/harness.cpp
    ${PROJECT_SOURCE_DIR}/src/algorithm/detail/sample_codec.cpp
    ${PROJECT_SOURCE_DIR}/src/pipeline/detail/operation_context_state.cpp
)
target_link_libraries(slideio_processor_algorithm_tests PRIVATE
    slideio-processor-engine slideio_processor_pipeline_test_support rapidjson GTest::gtest_main)
target_include_directories(slideio_processor_algorithm_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/src/algorithm
    ${PROJECT_SOURCE_DIR}/tests)

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
