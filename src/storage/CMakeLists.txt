add_library(slideio-processor-storage OBJECT
    store_format.cpp
)
set_target_properties(slideio-processor-storage PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_definitions(slideio-processor-storage PRIVATE SLIDEIO_PROCESSOR_DATA_API)
# SQLite is linked PRIVATE and never exported from the library.
target_link_libraries(slideio-processor-storage PRIVATE SQLite::SQLite3)
target_include_directories(slideio-processor-storage
    PUBLIC ${PROJECT_SOURCE_DIR}/include
    PRIVATE ${PROJECT_SOURCE_DIR}/src)
