include(GoogleTest)

add_executable(slideio_processor_core_tests
    test_error.cpp
    test_version.cpp
)
target_link_libraries(slideio_processor_core_tests PRIVATE
    slideio-processor-data GTest::gtest_main)
gtest_discover_tests(slideio_processor_core_tests)

# Every public header must compile on its own.
file(GLOB PUBLIC_HEADERS CONFIGURE_DEPENDS
    ${PROJECT_SOURCE_DIR}/include/slideio/processor/core/*.hpp)
set(HEADER_CHECK_DIR ${CMAKE_CURRENT_BINARY_DIR}/header_check)
set(HEADER_CHECK_SOURCES ${HEADER_CHECK_DIR}/main.cpp)
file(WRITE ${HEADER_CHECK_DIR}/main.cpp "int main() { return 0; }\n")
foreach(header ${PUBLIC_HEADERS})
    get_filename_component(name ${header} NAME_WE)
    set(src ${HEADER_CHECK_DIR}/check_${name}.cpp)
    file(GENERATE OUTPUT ${src}
        CONTENT "#include <slideio/processor/core/${name}.hpp>\n")
    list(APPEND HEADER_CHECK_SOURCES ${src})
endforeach()
add_executable(slideio_processor_header_check ${HEADER_CHECK_SOURCES})
target_include_directories(slideio_processor_header_check PRIVATE
    ${PROJECT_SOURCE_DIR}/include)

add_test(NAME public_headers_abi
    COMMAND ${CMAKE_COMMAND} -DHEADER_DIR=${PROJECT_SOURCE_DIR}/include
            -P ${PROJECT_SOURCE_DIR}/cmake/check_public_headers.cmake)
