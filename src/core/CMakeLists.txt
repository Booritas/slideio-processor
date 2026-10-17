add_library(slideio-processor-core OBJECT
    error.cpp
    version.cpp
)
set_target_properties(slideio-processor-core PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_definitions(slideio-processor-core PRIVATE SLIDEIO_PROCESSOR_DATA_API)
target_include_directories(slideio-processor-core
    PUBLIC ${PROJECT_SOURCE_DIR}/include
    PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(slideio-processor-core PRIVATE rapidjson)

set_source_files_properties(version.cpp PROPERTIES COMPILE_DEFINITIONS
    "SLIDEIO_PROCESSOR_VERSION_MAJOR=${PROJECT_VERSION_MAJOR};SLIDEIO_PROCESSOR_VERSION_MINOR=${PROJECT_VERSION_MINOR};SLIDEIO_PROCESSOR_VERSION_PATCH=${PROJECT_VERSION_PATCH};SLIDEIO_PROCESSOR_VERSION_STRING=\"${PROJECT_VERSION}\"")
