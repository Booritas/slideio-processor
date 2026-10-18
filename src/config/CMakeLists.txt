# The resolved OpenCV version, baked in at configure time. OpenCV itself is
# never linked: only its version is read.
configure_file(backend_version.hpp.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/config/backend_version.hpp @ONLY)

add_library(slideio-processor-config OBJECT
    backend_capabilities.cpp
    configuration.cpp
    detail/backend_types.cpp
)
set_target_properties(slideio-processor-config PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_definitions(slideio-processor-config PRIVATE SLIDEIO_PROCESSOR_DATA_API)
target_include_directories(slideio-processor-config
    PUBLIC ${PROJECT_SOURCE_DIR}/include
    PRIVATE ${PROJECT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_BINARY_DIR}/generated)
