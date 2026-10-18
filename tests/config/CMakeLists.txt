include(GoogleTest)

add_executable(slideio_processor_config_tests
    test_backend_capabilities.cpp
    test_configuration.cpp
    test_logging.cpp
    test_standard_vocabulary.cpp
    test_umbrella.cpp
    # Not exported from the library, so compiled into the test directly.
    ${PROJECT_SOURCE_DIR}/src/config/detail/backend_types.cpp
)
target_link_libraries(slideio_processor_config_tests PRIVATE
    slideio-processor-data GTest::gtest_main)
target_include_directories(slideio_processor_config_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/tests)
target_compile_definitions(slideio_processor_config_tests PRIVATE
    SLIDEIO_PROCESSOR_TEST_OPENCV_VERSION="${OpenCV_VERSION}")
# A hung waiter fails after 60 s instead of blocking the run.
gtest_discover_tests(slideio_processor_config_tests TEST_PREFIX "config." PROPERTIES TIMEOUT 60)

# Every public header must compile on its own.
file(GLOB CONFIG_PUBLIC_HEADERS CONFIGURE_DEPENDS
    ${PROJECT_SOURCE_DIR}/include/slideio/processor/config/*.hpp)
set(CONFIG_HEADER_CHECK_DIR ${CMAKE_CURRENT_BINARY_DIR}/header_check)
set(CONFIG_HEADER_CHECK_SOURCES ${CONFIG_HEADER_CHECK_DIR}/main.cpp)
file(WRITE ${CONFIG_HEADER_CHECK_DIR}/main.cpp "int main() { return 0; }\n")
foreach(header ${CONFIG_PUBLIC_HEADERS})
    get_filename_component(name ${header} NAME_WE)
    set(src ${CONFIG_HEADER_CHECK_DIR}/check_${name}.cpp)
    file(GENERATE OUTPUT ${src}
        CONTENT "#include <slideio/processor/config/${name}.hpp>\n")
    list(APPEND CONFIG_HEADER_CHECK_SOURCES ${src})
endforeach()
add_executable(slideio_processor_config_header_check ${CONFIG_HEADER_CHECK_SOURCES})
target_include_directories(slideio_processor_config_header_check PRIVATE
    ${PROJECT_SOURCE_DIR}/include)
