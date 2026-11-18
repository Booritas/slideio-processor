add_library(slideio-processor-algorithm OBJECT
    registry.cpp
    detail/bag.cpp
    detail/builtin_factory.cpp
    detail/colour_math.cpp
    detail/rgb_input.cpp
    detail/sample_codec.cpp
    pixel/colour_convert.cpp
    pixel/convert_type.cpp
    pixel/stain_deconvolution.cpp
    pixel/stain_vectors.cpp
    pixel/threshold_fixed.cpp
    pixel/threshold_otsu.cpp
)
set_target_properties(slideio-processor-algorithm PROPERTIES POSITION_INDEPENDENT_CODE ON)
# The sources are compiled into the engine, so the export switch is defined here.
target_compile_definitions(slideio-processor-algorithm PRIVATE SLIDEIO_PROCESSOR_ENGINE_API)
# OpenCV is confined to the engine library and stays PRIVATE: no OpenCV type,
# header or macro reaches a public header, which the header check in
# tests/pipeline verifies for algorithm/ as well.
target_link_libraries(slideio-processor-algorithm
    PUBLIC slideio-processor-data slideio::slideio
    PRIVATE opencv_core opencv_imgproc)
# Public headers, plus this module's own directory as the root of its
# "detail/..." includes. src/ itself is deliberately absent: the algorithm
# module is compiled against public headers only and cannot reach the
# pipeline's detail.
target_include_directories(slideio-processor-algorithm
    PUBLIC ${PROJECT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
