add_library(slideio-processor-storage OBJECT
    event_journal.cpp
    layer_registry.cpp
    object_cursor.cpp
    object_store.cpp
    object_writer.cpp
    stage_state_reader.cpp
    stage_state_writer.cpp
    store.cpp
    store_format.cpp
    vocabulary_registry.cpp
    detail/event_log.cpp
    detail/geometry_codec.cpp
    detail/label_aliases.cpp
    detail/polygon_predicates.cpp
    detail/query_builder.cpp
    detail/schema.cpp
    detail/sqlite.cpp
    detail/store_impl.cpp
    detail/writer_lock.cpp
)
# The wake-up signal (ruling S13): a named event on Windows; elsewhere a POSIX named
# semaphore (Linux) or no signal at all (macOS), both in the POSIX source.
if(WIN32)
    target_sources(slideio-processor-storage PRIVATE detail/change_signal_win32.cpp)
else()
    target_sources(slideio-processor-storage PRIVATE detail/change_signal_posix.cpp)
endif()
set_target_properties(slideio-processor-storage PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_definitions(slideio-processor-storage PRIVATE SLIDEIO_PROCESSOR_DATA_API)
# SQLite is linked PRIVATE and never exported from the library.
target_link_libraries(slideio-processor-storage PRIVATE SQLite::SQLite3 rapidjson)
target_include_directories(slideio-processor-storage
    PUBLIC ${PROJECT_SOURCE_DIR}/include
    PRIVATE ${PROJECT_SOURCE_DIR}/src)
