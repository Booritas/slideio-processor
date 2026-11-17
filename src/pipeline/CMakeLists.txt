add_library(slideio-processor-pipeline OBJECT
    backend.cpp
    level_selector.cpp
    operation.cpp
    operation_context.cpp
    operation_catalog.cpp
    operation_parameters.cpp
    pipeline.cpp
    pipeline_spec.cpp
    reduction.cpp
    standard_measurements.cpp
    tile_grid.cpp
    executor.cpp
    detail/combine_partials.cpp
    detail/fragment_merge.cpp
    detail/half.cpp
    detail/level_mapping.cpp
    detail/level_table.cpp
    detail/operation_context_state.cpp
    detail/parameters_json.cpp
    detail/plane_buffers.cpp
    detail/run_plan.cpp
    detail/restriction.cpp
    detail/scene_reader.cpp
    detail/tile_evaluator.cpp
    detail/type_contract.cpp
)
set_target_properties(slideio-processor-pipeline PROPERTIES POSITION_INDEPENDENT_CODE ON)
# The sources are compiled here, so the export switch is defined here.
target_compile_definitions(slideio-processor-pipeline PRIVATE SLIDEIO_PROCESSOR_ENGINE_API)
# OpenCV stays PRIVATE: no OpenCV type, header or macro reaches a public header
# (SDS 05 section 5.10, UR-BLD-4), which pipeline_headers_are_opencv_free checks.
target_link_libraries(slideio-processor-pipeline
    PUBLIC slideio-processor-data slideio::slideio
    PRIVATE opencv_core opencv_imgproc rapidjson)
target_include_directories(slideio-processor-pipeline
    PUBLIC ${PROJECT_SOURCE_DIR}/include
    PRIVATE ${PROJECT_SOURCE_DIR}/src)
