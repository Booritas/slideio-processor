add_library(slideio-processor-run OBJECT
    export_spec.cpp
    pipeline_file.cpp
    slide_run.cpp
    slide_run_result.cpp
)
set_target_properties(slideio-processor-run PROPERTIES POSITION_INDEPENDENT_CODE ON)
# The sources are compiled here, so the export switch is defined here.
target_compile_definitions(slideio-processor-run PRIVATE SLIDEIO_PROCESSOR_ENGINE_API)
# The run module depends on workspace, masks, pipeline and the data library; it never links
# algorithm, whose operations a run resolves by name through OperationCatalog (src/CMakeLists.txt
# fails the configure if it ever does).
target_link_libraries(slideio-processor-run
    PUBLIC slideio-processor-data slideio::slideio
    PRIVATE slideio-processor-workspace slideio-processor-masks slideio-processor-pipeline rapidjson)
target_include_directories(slideio-processor-run
    PUBLIC ${PROJECT_SOURCE_DIR}/include
    PRIVATE ${PROJECT_SOURCE_DIR}/src)
