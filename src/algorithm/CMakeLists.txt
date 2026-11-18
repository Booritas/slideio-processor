add_library(slideio-processor-algorithm OBJECT
    registry.cpp
    detail/bag.cpp
    detail/builtin_factory.cpp
    detail/colour_math.cpp
    detail/cv_plane.cpp
    detail/measurement_finish.cpp
    detail/region_emit.cpp
    detail/region_trace.cpp
    detail/rgb_input.cpp
    detail/sample_codec.cpp
    detail/separable.cpp
    measure/measure.cpp
    pixel/bilateral_filter.cpp
    pixel/canny.cpp
    pixel/colour_convert.cpp
    pixel/convert_type.cpp
    pixel/derivatives.cpp
    pixel/gaussian_blur.cpp
    pixel/median_blur.cpp
    pixel/morphology.cpp
    pixel/stain_deconvolution.cpp
    pixel/stain_vectors.cpp
    pixel/structuring_element.cpp
    pixel/threshold_adaptive.cpp
    pixel/threshold_fixed.cpp
    pixel/threshold_otsu.cpp
    segment/connected_components.cpp
)
set_target_properties(slideio-processor-algorithm PROPERTIES POSITION_INDEPENDENT_CODE ON)
# The sources are compiled into the engine, so the export switch is defined here.
target_compile_definitions(slideio-processor-algorithm PRIVATE SLIDEIO_PROCESSOR_ENGINE_API)
# Results are compared bit for bit across tile sizes and against direct
# references, so a multiply and an add must never be fused into an FMA in one
# place and not another. MSVC does not contract by default.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(slideio-processor-algorithm PRIVATE -ffp-contract=off)
endif()
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
