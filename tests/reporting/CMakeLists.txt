include(GoogleTest)

add_executable(slideio_processor_reporting_tests
    test_materialise.cpp
    test_column_schema.cpp
    test_csv_writer.cpp
)
# store_fixture.hpp (header-only) comes from tests/storage through ${PROJECT_SOURCE_DIR}/tests.
target_link_libraries(slideio_processor_reporting_tests PRIVATE slideio-processor-data GTest::gtest_main)
# test_column_schema.cpp reaches detail::resolveColumns, exported only with test seams on.
target_include_directories(slideio_processor_reporting_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/tests)
gtest_discover_tests(slideio_processor_reporting_tests DISCOVERY_TIMEOUT 60 TEST_PREFIX "reporting." PROPERTIES TIMEOUT 60)
