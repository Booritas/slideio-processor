include(GoogleTest)

# Another process on a store: an observer for test_cross_process.cpp, and a raw SQL
# writer for tests that need a foreign writer beside a live store (helper_process.hpp).
# It lands in the shared runtime output directory beside slideio-processor-data, so it
# finds the library. SQLite is linked directly for the raw writer.
add_executable(store_observer_helper store_observer_helper.cpp)
target_link_libraries(store_observer_helper PRIVATE slideio-processor-data SQLite::SQLite3)

if(WIN32)
    set(STORAGE_CHANGE_SIGNAL_SOURCE change_signal_win32.cpp)
else()
    set(STORAGE_CHANGE_SIGNAL_SOURCE change_signal_posix.cpp)
endif()

add_executable(slideio_processor_storage_tests
    test_cross_process.cpp
    test_event_journal.cpp
    test_stage_state.cpp
    test_geometry_codec.cpp
    test_layer_registry.cpp
    test_object_query.cpp
    test_object_writer.cpp
    test_store.cpp
    test_store_impl.cpp
    test_types.cpp
    test_umbrella.cpp
    test_vocabulary_registry.cpp
    # Not exported from the library, so compiled into the test directly.
    ${PROJECT_SOURCE_DIR}/src/storage/detail/${STORAGE_CHANGE_SIGNAL_SOURCE}
    ${PROJECT_SOURCE_DIR}/src/storage/detail/event_log.cpp
    ${PROJECT_SOURCE_DIR}/src/storage/detail/geometry_codec.cpp
    ${PROJECT_SOURCE_DIR}/src/storage/detail/label_aliases.cpp
    ${PROJECT_SOURCE_DIR}/src/storage/detail/polygon_predicates.cpp
    ${PROJECT_SOURCE_DIR}/src/storage/detail/schema.cpp
    ${PROJECT_SOURCE_DIR}/src/storage/detail/sqlite.cpp
    ${PROJECT_SOURCE_DIR}/src/storage/detail/store_impl.cpp
    ${PROJECT_SOURCE_DIR}/src/storage/detail/writer_lock.cpp
)
# SQLite is linked directly to probe the R*Tree module of the Conan build.
target_link_libraries(slideio_processor_storage_tests PRIVATE
    slideio-processor-data SQLite::SQLite3 rapidjson Threads::Threads GTest::gtest_main)
target_include_directories(slideio_processor_storage_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/tests)
add_dependencies(slideio_processor_storage_tests store_observer_helper)
target_compile_definitions(slideio_processor_storage_tests PRIVATE
    SLIDEIO_PROCESSOR_TEST_OBSERVER_HELPER="$<TARGET_FILE:store_observer_helper>")
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
