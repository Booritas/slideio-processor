add_subdirectory(core)
add_subdirectory(config)
add_subdirectory(storage)
add_subdirectory(reporting)
# A data-only build (SLIDEIO_PROCESSOR_DATA_ONLY) has no engine to test.
if(NOT SLIDEIO_PROCESSOR_DATA_ONLY)
    # The engine-side tests load SlideIO's DLLs, which live in SlideIO's install
    # tree. On Linux and macOS the imported library's RPATH finds them. On
    # Windows they are copied beside the test executable after each build, before
    # gtest_discover_tests runs it (the default POST_BUILD discovery, as for the
    # data tests), so discovery and every test find them, and ctest runs each
    # test executable directly. They used to reach PATH through a `cmake -E env`
    # TEST_LAUNCHER, with discovery at ctest time (PRE_TEST), which runs every
    # test through `cmake -P LaunchTest.cmake`: two cmake.exe per test, about
    # 2800 a run, and two runs at once made cmake.exe fail to start (0xc0000142)
    # with an error box per failure.
    if(WIN32)
        get_target_property(SLIDEIO_RUNTIME slideio::slideio IMPORTED_LOCATION_RELEASE)
        if(NOT SLIDEIO_RUNTIME)
            # SlideIO's hand-written package config sets the configuration-less property.
            get_target_property(SLIDEIO_RUNTIME slideio::slideio IMPORTED_LOCATION)
        endif()
        if(NOT SLIDEIO_RUNTIME)
            message(FATAL_ERROR "tests: slideio::slideio has no imported location")
        endif()
        get_filename_component(SLIDEIO_BIN_DIR "${SLIDEIO_RUNTIME}" DIRECTORY)
        # Every DLL of SlideIO's bin directory: SlideIO's drivers and the
        # libraries they load are found beside the executable, as PATH did.
        file(GLOB SLIDEIO_RUNTIME_DLLS "${SLIDEIO_BIN_DIR}/*.dll")
        if(NOT SLIDEIO_RUNTIME_DLLS)
            message(FATAL_ERROR "tests: no DLL in SlideIO's bin directory ${SLIDEIO_BIN_DIR}")
        endif()
    endif()
    # Copies SlideIO's DLLs beside @p target's executable after it is built
    # (Windows only; elsewhere nothing). Call it before gtest_discover_tests():
    # POST_BUILD commands run in the order they are added.
    function(slideio_processor_copy_slideio_runtime target)
        if(WIN32)
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SLIDEIO_RUNTIME_DLLS} "$<TARGET_FILE_DIR:${target}>"
                VERBATIM)
        endif()
    endfunction()

    add_subdirectory(pipeline)
    add_subdirectory(algorithm)
    add_subdirectory(workspace)
    add_subdirectory(masks)
    add_subdirectory(run)
endif()
