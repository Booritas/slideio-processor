include(GoogleTest)

add_executable(slideio_processor_storage_tests
    test_types.cpp
)
# SQLite is linked directly to probe the R*Tree module of the Conan build.
target_link_libraries(slideio_processor_storage_tests PRIVATE
    slideio-processor-data SQLite::SQLite3 GTest::gtest_main)
target_include_directories(slideio_processor_storage_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/tests)
# A hung waiter fails after 60 s instead of blocking the run.
gtest_discover_tests(slideio_processor_storage_tests TEST_PREFIX "storage." PROPERTIES TIMEOUT 60)

# Every public header must compile on its own.
file(GLOB STORAGE_PUBLIC_HEADERS CONFIGURE_DEPENDS
    ${PROJECT_SOURCE_DIR}/include/slideio/processor/storage/*.hpp)
set(STORAGE_HEADER_CHECK_DIR ${CMAKE_CURRENT_BINARY_DIR}/header_check)
set(STORAGE_HEADER_CHECK_SOURCES ${STORAGE_HEADER_CHECK_DIR}/main.cpp)
file(WRITE ${STORAGE_HEADER_CHECK_DIR}/main.cpp "int main() { return 0; }\n")
foreach(header ${STORAGE_PUBLIC_HEADERS})
    get_filename_component(name ${header} NAME_WE)
    set(src ${STORAGE_HEADER_CHECK_DIR}/check_${name}.cpp)
    file(GENERATE OUTPUT ${src}
        CONTENT "#include <slideio/processor/storage/${name}.hpp>\n")
    list(APPEND STORAGE_HEADER_CHECK_SOURCES ${src})
endforeach()
add_executable(slideio_processor_storage_header_check ${STORAGE_HEADER_CHECK_SOURCES})
target_include_directories(slideio_processor_storage_header_check PRIVATE
    ${PROJECT_SOURCE_DIR}/include)
