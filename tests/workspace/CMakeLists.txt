include(GoogleTest)

# The workspace tests write their slides with libtiff (support/test_slide.cpp), so
# SlideIO opens a real file through its own driver lookup.
add_executable(slideio_processor_workspace_tests
    support/test_slide.cpp
    test_workspace.cpp
)
target_link_libraries(slideio_processor_workspace_tests PRIVATE
    slideio-processor-engine TIFF::TIFF GTest::gtest_main)
target_include_directories(slideio_processor_workspace_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/tests)

# The tests load SlideIO's DLLs, which live in SlideIO's install tree rather than
# beside the test; as tests/pipeline/CMakeLists.txt explains, on Windows each test and
# the ctest-time discovery get that bin directory prepended to PATH.
set(WORKSPACE_TEST_PROPERTIES TIMEOUT 60)
if(WIN32)
    if(CMAKE_VERSION VERSION_LESS 3.29)
        message(WARNING "tests/workspace: CMake ${CMAKE_VERSION} ignores TEST_LAUNCHER; the workspace tests "
                        "need CMake 3.29 or later on Windows to find SlideIO's DLLs.")
    endif()
    get_target_property(SLIDEIO_RUNTIME slideio::slideio IMPORTED_LOCATION_RELEASE)
    if(NOT SLIDEIO_RUNTIME)
        # SlideIO's hand-written package config sets the configuration-less property.
        get_target_property(SLIDEIO_RUNTIME slideio::slideio IMPORTED_LOCATION)
    endif()
    if(NOT SLIDEIO_RUNTIME)
        message(FATAL_ERROR "tests/workspace: slideio::slideio has no imported location")
    endif()
    get_filename_component(SLIDEIO_BIN_DIR "${SLIDEIO_RUNTIME}" DIRECTORY)
    file(TO_NATIVE_PATH "${SLIDEIO_BIN_DIR}" SLIDEIO_BIN_DIR)
    list(APPEND WORKSPACE_TEST_PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${SLIDEIO_BIN_DIR}")
    set_property(TARGET slideio_processor_workspace_tests PROPERTY TEST_LAUNCHER
        ${CMAKE_COMMAND} -E env --modify "PATH=path_list_prepend:${SLIDEIO_BIN_DIR}" --)
endif()
gtest_discover_tests(slideio_processor_workspace_tests TEST_PREFIX "workspace."
    DISCOVERY_MODE PRE_TEST
    PROPERTIES ${WORKSPACE_TEST_PROPERTIES})
