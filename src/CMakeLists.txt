add_subdirectory(core)
add_subdirectory(config)
add_subdirectory(storage)
add_subdirectory(pipeline)
add_subdirectory(algorithm)
add_subdirectory(workspace)

# Linking the object library (rather than $<TARGET_OBJECTS:...>) puts its
# objects into the DLL and lets its usage requirements propagate.
add_library(slideio-processor-data SHARED)
target_link_libraries(slideio-processor-data PRIVATE
    slideio-processor-core
    slideio-processor-config
    slideio-processor-storage)
target_include_directories(slideio-processor-data PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>)

# The static SQLite inside the library must not interpose on, or be interposed
# by, the copy SlideIO's converter carries.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_options(slideio-processor-data PRIVATE -Wl,--exclude-libs,ALL)
endif()

# OpenCV is used for its version only, and SlideIO for its headers only: its DLLs
# embed OpenCV statically (design 5.1, ruling S40). Fail the configure if either
# ever reaches the link line of the library or of any of its modules. Our own
# slideio-processor-* targets do not match "^slideio::".
foreach(target slideio-processor-config slideio-processor-storage slideio-processor-data)
    get_target_property(link_libs ${target} LINK_LIBRARIES)
    get_target_property(interface_link_libs ${target} INTERFACE_LINK_LIBRARIES)
    foreach(libs ${link_libs} ${interface_link_libs})
        if(libs MATCHES "[Oo][Pp][Ee][Nn][Cc][Vv]")
            message(FATAL_ERROR
                "${target} must not link OpenCV (found '${libs}'): "
                "OpenCV is a version-only dependency.")
        endif()
        if(libs MATCHES "^slideio::")
            message(FATAL_ERROR
                "${target} must not link SlideIO (found '${libs}'): "
                "its DLLs embed OpenCV, which the data library must not carry.")
        endif()
    endforeach()
endforeach()

# The engine library (design 5.1): pipeline, algorithm and workspace. Its
# public headers name slideio::Scene and the data library's types,
# so both are PUBLIC; OpenCV is static and PRIVATE. SlideIO's DLLs carry
# their own static OpenCV, and the two copies never meet: SlideIO's API is
# buffer-based, so no cv:: object crosses it.
add_library(slideio-processor-engine SHARED)
target_link_libraries(slideio-processor-engine
    PUBLIC slideio-processor-data slideio::slideio
    PRIVATE slideio-processor-pipeline slideio-processor-algorithm slideio-processor-workspace opencv_core opencv_imgproc)
target_include_directories(slideio-processor-engine PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>)
target_compile_definitions(slideio-processor-engine PRIVATE SLIDEIO_PROCESSOR_ENGINE_API)

# The engine passes STL types across SlideIO's DLL boundary, so under MSVC its
# configuration must match SlideIO's (Debug with SlideIO's _d libraries, anything
# else with its Release ones). A single-config build is checked here; for a
# multi-config generator, which picks the configuration at build time, the guard
# target fails the build of a mismatched configuration before the engine compiles.
if(MSVC)
    get_target_property(SLIDEIO_IMPLIB slideio::slideio IMPORTED_IMPLIB)
    if(NOT SLIDEIO_IMPLIB)
        message(FATAL_ERROR "src: slideio::slideio has no import library")
    endif()
    set(SLIDEIO_LIBRARY "${SLIDEIO_IMPLIB}")
    set(CONFIG "${CMAKE_BUILD_TYPE}")
    include(${PROJECT_SOURCE_DIR}/cmake/check_slideio_configuration.cmake)
    add_custom_target(slideio-configuration-guard
        COMMAND ${CMAKE_COMMAND} -DCONFIG=$<CONFIG> -DSLIDEIO_LIBRARY=${SLIDEIO_IMPLIB}
                -P ${PROJECT_SOURCE_DIR}/cmake/check_slideio_configuration.cmake
        VERBATIM)
    add_dependencies(slideio-processor-engine slideio-configuration-guard)
endif()

# The static OpenCV inside the engine must not interpose on, or be interposed by,
# the copy SlideIO's libraries carry.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_options(slideio-processor-engine PRIVATE -Wl,--exclude-libs,ALL)
endif()
