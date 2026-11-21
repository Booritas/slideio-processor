add_library(slideio-processor-masks OBJECT
    label_scene.cpp
    mask_writer.cpp
)
set_target_properties(slideio-processor-masks PROPERTIES POSITION_INDEPENDENT_CODE ON)
# The sources are compiled here, so the export switch is defined here.
target_compile_definitions(slideio-processor-masks PRIVATE SLIDEIO_PROCESSOR_ENGINE_API)
# The masks module depends on the data library (core, config, storage, reporting) and
# SlideIO only: never on algorithm, workspace or OpenCV (design 5.1). It is an engine
# module because it links SlideIO, whose DLLs embed OpenCV; the data library never does.
target_link_libraries(slideio-processor-masks
    PUBLIC slideio-processor-data slideio::slideio)
target_include_directories(slideio-processor-masks
    PUBLIC ${PROJECT_SOURCE_DIR}/include
    PRIVATE ${PROJECT_SOURCE_DIR}/src)
