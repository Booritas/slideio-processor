add_library(slideio-processor-reporting OBJECT
    export_result.cpp
    materialise.cpp
    column_schema.cpp
    csv_writer.cpp
    detail/columns.cpp
    detail/csv_format.cpp
    detail/wkt.cpp
)
set_target_properties(slideio-processor-reporting PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_definitions(slideio-processor-reporting PRIVATE SLIDEIO_PROCESSOR_DATA_API)
# Reporting reads through core, config and storage headers only: never pipeline,
# algorithm, workspace or OpenCV.
target_include_directories(slideio-processor-reporting
    PUBLIC ${PROJECT_SOURCE_DIR}/include
    PRIVATE ${PROJECT_SOURCE_DIR}/src)
