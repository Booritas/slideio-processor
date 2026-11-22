include(GoogleTest)

# The pipeline-file tests build a development workspace over a libtiff-written slide
# (tests/workspace/support), so SlideIO opens a real file.
add_executable(slideio_processor_run_tests
    ${PROJECT_SOURCE_DIR}/tests/workspace/support/test_slide.cpp
    test_pipeline_file.cpp
)
target_link_libraries(slideio_processor_run_tests PRIVATE
    slideio-processor-engine TIFF::TIFF GTest::gtest_main)
target_include_directories(slideio_processor_run_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/tests)

# The tests load SlideIO's DLLs, which live in SlideIO's install tree rather than beside the
# test; as tests/pipeline/CMakeLists.txt explains, on Windows each test and the ctest-time
# discovery get that bin directory prepended to PATH.
set(RUN_TEST_PROPERTIES TIMEOUT 60)
if(WIN32)
    if(CMAKE_VERSION VERSION_LESS 3.29)
        message(WARNING "tests/run: CMake ${CMAKE_VERSION} ignores TEST_LAUNCHER; the run tests "
                        "need CMake 3.29 or later on Windows to find SlideIO's DLLs.")
    endif()
    get_target_property(SLIDEIO_RUNTIME slideio::slideio IMPORTED_LOCATION_RELEASE)
    if(NOT SLIDEIO_RUNTIME)
        # SlideIO's hand-written package config sets the configuration-less property.
        get_target_property(SLIDEIO_RUNTIME slideio::slideio IMPORTED_LOCATION)
    endif()
    if(NOT SLIDEIO_RUNTIME)
        message(FATAL_ERROR "tests/run: slideio::slideio has no imported location")
    endif()
    get_filename_component(SLIDEIO_BIN_DIR "${SLIDEIO_RUNTIME}" DIRECTORY)
    file(TO_NATIVE_PATH "${SLIDEIO_BIN_DIR}" SLIDEIO_BIN_DIR)
    list(APPEND RUN_TEST_PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${SLIDEIO_BIN_DIR}")
    set_property(TARGET slideio_processor_run_tests PROPERTY TEST_LAUNCHER
        ${CMAKE_COMMAND} -E env --modify "PATH=path_list_prepend:${SLIDEIO_BIN_DIR}" --)
endif()
gtest_discover_tests(slideio_processor_run_tests DISCOVERY_TIMEOUT 60 TEST_PREFIX "run."
    DISCOVERY_MODE PRE_TEST
    PROPERTIES ${RUN_TEST_PROPERTIES})

# Every public header of the run module must compile on its own, with the SlideIO and data
# include paths but no OpenCV include path. A new header without its check source would
# escape the check, so configuring fails until header_check/check_<name>.cpp exists for every
# include/slideio/processor/run/<name>.hpp.
add_library(run_headers_are_opencv_free OBJECT
    header_check/check_export_spec.cpp
    header_check/check_pipeline_file.cpp
    header_check/check_run.cpp
)
file(GLOB RUN_PUBLIC_HEADERS RELATIVE "${PROJECT_SOURCE_DIR}/include/slideio/processor/run"
    CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/include/slideio/processor/run/*.hpp")
get_target_property(RUN_HEADER_CHECK_SOURCES run_headers_are_opencv_free SOURCES)
foreach(header IN LISTS RUN_PUBLIC_HEADERS)
    string(REGEX REPLACE "\.hpp$" "" stem "${header}")
    list(FIND RUN_HEADER_CHECK_SOURCES "header_check/check_${stem}.cpp" listed)
    if(listed EQUAL -1 OR NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/header_check/check_${stem}.cpp")
        message(FATAL_ERROR "tests/run: include/slideio/processor/run/${header} has no header check; "
            "add header_check/check_${stem}.cpp and list it in run_headers_are_opencv_free")
    endif()
endforeach()
target_include_directories(run_headers_are_opencv_free PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    $<TARGET_PROPERTY:slideio::slideio,INTERFACE_INCLUDE_DIRECTORIES>)
