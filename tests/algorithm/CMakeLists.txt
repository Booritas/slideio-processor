include(GoogleTest)

# "test.disks", a LabelImageOperation over fixed disks with tile-dependent labels,
# in a library of its own so that later test targets (tests/run) can run it too.
add_library(slideio_processor_algorithm_test_support STATIC
    support/disks_operation.cpp
)
target_link_libraries(slideio_processor_algorithm_test_support PUBLIC
    slideio-processor-engine slideio_processor_pipeline_test_support)
target_include_directories(slideio_processor_algorithm_test_support PUBLIC ${PROJECT_SOURCE_DIR}/tests)

# The algorithm tests reach the pipeline's detail headers (the operation context,
# parameter comparison) and the algorithm's own sample codec, colour maths and
# OpenCV plane helpers, so src/ and tests/ are on their include path; the
# algorithm library itself never has src/ on its path. Detail code the engine
# does not export is compiled in directly.
add_executable(slideio_processor_algorithm_tests
    test_registry.cpp
    test_colour_convert.cpp
    test_stain_deconvolution.cpp
    test_convert_type.cpp
    test_threshold_fixed.cpp
    test_threshold_otsu.cpp
    test_threshold_adaptive.cpp
    test_gaussian_blur.cpp
    test_derivatives.cpp
    test_median_blur.cpp
    test_distance_transform.cpp
    test_fill_holes.cpp
    test_remove_large_objects.cpp
    test_nuclei_chain.cpp
    test_morphology.cpp
    test_bilateral_filter.cpp
    test_canny.cpp
    test_cv_plane.cpp
    test_region_trace.cpp
    test_measurement_finish.cpp
    test_connected_components.cpp
    test_contours.cpp
    test_watershed.cpp
    test_flood_model.cpp
    test_tissue_detect.cpp
    test_label_image.cpp
    support/harness.cpp
    support/flood_model.cpp
    # The label-image tests run a stage in a workspace over a slide file written
    # with libtiff, as the workspace tests do.
    ${PROJECT_SOURCE_DIR}/tests/workspace/support/test_slide.cpp
    ${PROJECT_SOURCE_DIR}/src/algorithm/detail/colour_math.cpp
    ${PROJECT_SOURCE_DIR}/src/algorithm/detail/sample_codec.cpp
    ${PROJECT_SOURCE_DIR}/src/algorithm/detail/cv_plane.cpp
    ${PROJECT_SOURCE_DIR}/src/algorithm/detail/measurement_finish.cpp
    ${PROJECT_SOURCE_DIR}/src/algorithm/detail/region_emit.cpp
    ${PROJECT_SOURCE_DIR}/src/algorithm/detail/region_trace.cpp
    ${PROJECT_SOURCE_DIR}/src/pipeline/detail/operation_context_state.cpp
)
target_link_libraries(slideio_processor_algorithm_tests PRIVATE
    slideio-processor-engine slideio_processor_pipeline_test_support slideio_processor_algorithm_test_support
    TIFF::TIFF rapidjson GTest::gtest_main)
# The derivative tests compare their coefficients with OpenCV's own kernels and
# filters, so OpenCV's image processing is linked here, for the tests only.
target_link_libraries(slideio_processor_algorithm_tests PRIVATE opencv_core opencv_imgproc)
target_include_directories(slideio_processor_algorithm_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/src/algorithm
    ${PROJECT_SOURCE_DIR}/tests)
# The tests compare floating-point results bit for bit with direct references
# and across tile sizes, so neither side may fuse a multiply and an add into
# an FMA the other does not. MSVC does not contract by default.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(slideio_processor_algorithm_tests PRIVATE -ffp-contract=off)
endif()

# SlideIO's DLLs are found beside the executable (tests/CMakeLists.txt).
slideio_processor_copy_slideio_runtime(slideio_processor_algorithm_tests)
gtest_discover_tests(slideio_processor_algorithm_tests DISCOVERY_TIMEOUT 60 TEST_PREFIX "algorithm."
    PROPERTIES TIMEOUT 120)

# Working-set tests measure what an operation allocates through operator new,
# which this executable replaces with a counting pair (support/allocation_meter).
# A shared library may allocate through its own operator new (under MSVC every
# module has its own), so everything an operation's apply() can allocate through
# is compiled into this executable instead of being taken from the libraries:
# the engine's pipeline and algorithm code, and the data library's core and
# configuration, whose Polygon, ObjectRecord and MeasurementSet hold the
# objects an operation emits. The pipeline and algorithm sources are compiled
# again here, with both export switches, so every engine and data header
# declares its symbols as defined here rather than imported; core and
# configuration are compiled with the data switch already. The data library's
# storage is not needed, and SlideIO is still linked as a library: it allocates
# nothing an operation measures.
set(WORKING_SET_ENGINE_SOURCES)
foreach(library slideio-processor-pipeline slideio-processor-algorithm)
    get_target_property(library_dir ${library} SOURCE_DIR)
    get_target_property(library_sources ${library} SOURCES)
    foreach(source IN LISTS library_sources)
        list(APPEND WORKING_SET_ENGINE_SOURCES "${library_dir}/${source}")
    endforeach()
endforeach()
add_executable(slideio_processor_working_set_tests
    test_allocation_meter.cpp
    test_working_set_pixel.cpp
    test_working_set_filter.cpp
    test_working_set_segment.cpp
    support/allocation_meter.cpp
    support/working_set_check.cpp
    support/harness.cpp
    ${PROJECT_SOURCE_DIR}/tests/pipeline/test_support/synthetic_scene.cpp
    ${WORKING_SET_ENGINE_SOURCES}
    $<TARGET_OBJECTS:slideio-processor-core>
    $<TARGET_OBJECTS:slideio-processor-config>
)
target_compile_definitions(slideio_processor_working_set_tests PRIVATE
    SLIDEIO_PROCESSOR_ENGINE_API SLIDEIO_PROCESSOR_DATA_API)
target_link_libraries(slideio_processor_working_set_tests PRIVATE
    slideio::slideio opencv_core opencv_imgproc rapidjson GTest::gtest_main)
target_include_directories(slideio_processor_working_set_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/src/algorithm
    ${PROJECT_SOURCE_DIR}/tests)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(slideio_processor_working_set_tests PRIVATE -ffp-contract=off)
endif()
# As the engine does: the static OpenCV compiled in here must not interpose on,
# or be interposed by, the copy SlideIO's libraries carry.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_options(slideio_processor_working_set_tests PRIVATE -Wl,--exclude-libs,ALL)
endif()
slideio_processor_copy_slideio_runtime(slideio_processor_working_set_tests)
gtest_discover_tests(slideio_processor_working_set_tests DISCOVERY_TIMEOUT 60 TEST_PREFIX "algorithm.ws."
    PROPERTIES TIMEOUT 120)
