add_subdirectory(core)

add_library(slideio-processor-data SHARED $<TARGET_OBJECTS:slideio-processor-core>)
target_include_directories(slideio-processor-data PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>)
target_link_libraries(slideio-processor-data PRIVATE rapidjson)
