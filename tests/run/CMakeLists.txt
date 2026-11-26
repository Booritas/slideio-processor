include(GoogleTest)

# The pipeline-file and slide-run tests build workspaces over libtiff-written slides
# (tests/workspace/support), so SlideIO opens a real file. The slide-run tests also run
# "test.disks", the LabelImageOperation of tests/algorithm/support: the test target, never
# the run library, links the algorithm test support (built-ins register when the engine loads).
add_executable(slideio_processor_run_tests
    ${PROJECT_SOURCE_DIR}/tests/workspace/support/test_slide.cpp
    test_pipeline_file.cpp
    test_slide_run.cpp
    test_workspace_export.cpp
)
target_link_libraries(slideio_processor_run_tests PRIVATE
    slideio-processor-engine slideio_processor_algorithm_test_support TIFF::TIFF GTest::gtest_main)
target_include_directories(slideio_processor_run_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/tests)

# SlideIO's DLLs are found beside the executable (tests/CMakeLists.txt).
slideio_processor_copy_slideio_runtime(slideio_processor_run_tests)
gtest_discover_tests(slideio_processor_run_tests DISCOVERY_TIMEOUT 60 TEST_PREFIX "run."
    PROPERTIES TIMEOUT 60)

# Every public header of the run module must compile on its own, with the SlideIO and data
# include paths but no OpenCV include path. A new header without its check source would
# escape the check, so configuring fails until header_check/check_<name>.cpp exists for every
# include/slideio/processor/run/<name>.hpp.
add_library(run_headers_are_opencv_free OBJECT
    header_check/check_export_spec.cpp
    header_check/check_pipeline_file.cpp
    header_check/check_run.cpp
    header_check/check_slide_run.cpp
    header_check/check_slide_run_result.cpp
    header_check/check_workspace_export.cpp
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
