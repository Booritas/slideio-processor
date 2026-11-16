add_library(slideio-processor-pipeline OBJECT
    backend.cpp
)
set_target_properties(slideio-processor-pipeline PROPERTIES POSITION_INDEPENDENT_CODE ON)
# The sources are compiled here, so the export switch is defined here (ruling P1).
target_compile_definitions(slideio-processor-pipeline PRIVATE SLIDEIO_PROCESSOR_ENGINE_API)
# OpenCV stays PRIVATE: no OpenCV type, header or macro reaches a public header
# (SDS 05 section 5.10, UR-BLD-4), which pipeline_headers_are_opencv_free checks.
target_link_libraries(slideio-processor-pipeline
    PUBLIC slideio-processor-data slideio::slideio
    PRIVATE opencv_core opencv_imgproc)
target_include_directories(slideio-processor-pipeline
    PUBLIC ${PROJECT_SOURCE_DIR}/include
    PRIVATE ${PROJECT_SOURCE_DIR}/src)
