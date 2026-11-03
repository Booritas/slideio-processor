add_subdirectory(core)
add_subdirectory(config)
add_subdirectory(storage)

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

# OpenCV is used for its version only. Fail the configure if it ever reaches
# the link line of the library or of any of its modules.
foreach(target slideio-processor-config slideio-processor-storage slideio-processor-data)
    get_target_property(link_libs ${target} LINK_LIBRARIES)
    get_target_property(interface_link_libs ${target} INTERFACE_LINK_LIBRARIES)
    foreach(libs ${link_libs} ${interface_link_libs})
        if(libs MATCHES "[Oo][Pp][Ee][Nn][Cc][Vv]")
            message(FATAL_ERROR
                "${target} must not link OpenCV (found '${libs}'): "
                "OpenCV is a version-only dependency.")
        endif()
    endforeach()
endforeach()
