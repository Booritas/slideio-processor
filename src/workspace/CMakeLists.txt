add_library(slideio-processor-workspace OBJECT
    layer_lifecycle.cpp
    stage_registry.cpp
    stage_definition.cpp
    detail/stage_record.cpp
    workspace.cpp
)
set_target_properties(slideio-processor-workspace PROPERTIES POSITION_INDEPENDENT_CODE ON)
# The sources are compiled here, so the export switch is defined here.
target_compile_definitions(slideio-processor-workspace PRIVATE SLIDEIO_PROCESSOR_ENGINE_API)
# The workspace depends on core, config, storage and pipeline only; it never links
# algorithm, whose operations it resolves by name through OperationCatalog.
target_link_libraries(slideio-processor-workspace
    PUBLIC slideio-processor-data slideio::slideio
    PRIVATE rapidjson)
target_include_directories(slideio-processor-workspace
    PUBLIC ${PROJECT_SOURCE_DIR}/include
    PRIVATE ${PROJECT_SOURCE_DIR}/src)
