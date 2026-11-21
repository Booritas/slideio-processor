include(GoogleTest)

# The masks module's tests (reporting plan C). They link SlideIO, so they are an engine-side
# target; the data library never does. test_ome_layout.cpp writes its raster through
# detail::writePyramidTiff, which the data library does not export, so its sources are
# compiled in here as tests/storage does, with libtiff, zlib and rapidjson linked directly.
add_executable(slideio_processor_masks_tests
    test_ome_layout.cpp
    ${PROJECT_SOURCE_DIR}/src/storage/detail/file_io.cpp
    ${PROJECT_SOURCE_DIR}/src/storage/detail/half_float.cpp
    ${PROJECT_SOURCE_DIR}/src/storage/detail/pyramid_tiff_writer.cpp
    ${PROJECT_SOURCE_DIR}/src/storage/detail/tiff_io.cpp
    ${PROJECT_SOURCE_DIR}/src/storage/detail/tile_spool.cpp
)
target_link_libraries(slideio_processor_masks_tests PRIVATE
    slideio-processor-engine slideio::slideio slideio-processor-data
    rapidjson TIFF::TIFF ZLIB::ZLIB GTest::gtest_main)
target_include_directories(slideio_processor_masks_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/tests)

# The tests load SlideIO's DLLs, which live in SlideIO's install tree rather than beside the
# test; as tests/pipeline/CMakeLists.txt explains, on Windows each test and the ctest-time
# discovery get that bin directory prepended to PATH.
set(MASKS_TEST_PROPERTIES TIMEOUT 60)
if(WIN32)
    if(CMAKE_VERSION VERSION_LESS 3.29)
        message(WARNING "tests/masks: CMake ${CMAKE_VERSION} ignores TEST_LAUNCHER; the masks tests "
                        "need CMake 3.29 or later on Windows to find SlideIO's DLLs.")
    endif()
    get_target_property(SLIDEIO_RUNTIME slideio::slideio IMPORTED_LOCATION_RELEASE)
    if(NOT SLIDEIO_RUNTIME)
        # SlideIO's hand-written package config sets the configuration-less property.
        get_target_property(SLIDEIO_RUNTIME slideio::slideio IMPORTED_LOCATION)
    endif()
    if(NOT SLIDEIO_RUNTIME)
        message(FATAL_ERROR "tests/masks: slideio::slideio has no imported location")
    endif()
    get_filename_component(SLIDEIO_BIN_DIR "${SLIDEIO_RUNTIME}" DIRECTORY)
    file(TO_NATIVE_PATH "${SLIDEIO_BIN_DIR}" SLIDEIO_BIN_DIR)
    list(APPEND MASKS_TEST_PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${SLIDEIO_BIN_DIR}")
    set_property(TARGET slideio_processor_masks_tests PROPERTY TEST_LAUNCHER
        ${CMAKE_COMMAND} -E env --modify "PATH=path_list_prepend:${SLIDEIO_BIN_DIR}" --)
endif()
gtest_discover_tests(slideio_processor_masks_tests DISCOVERY_TIMEOUT 60 TEST_PREFIX "masks."
    DISCOVERY_MODE PRE_TEST
    PROPERTIES ${MASKS_TEST_PROPERTIES})
