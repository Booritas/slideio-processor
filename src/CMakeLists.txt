add_subdirectory(core)

# Linking the object library (rather than $<TARGET_OBJECTS:...>) puts its
# objects into the DLL and lets its usage requirements propagate.
add_library(slideio-processor-data SHARED)
target_link_libraries(slideio-processor-data PRIVATE slideio-processor-core)
target_include_directories(slideio-processor-data PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>)
