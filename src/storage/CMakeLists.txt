add_library(slideio-processor-storage OBJECT
    annotation_store.cpp
    annotation_writer.cpp
    edge_store.cpp
    edge_writer.cpp
    geojson_import.cpp
    event_journal.cpp
    label_raster_source.cpp
    layer_registry.cpp
    object_cursor.cpp
    object_store.cpp
    object_writer.cpp
    raster_declaration.cpp
    raster_registry.cpp
    raster_writer.cpp
    stage_state_reader.cpp
    stage_state_writer.cpp
    store.cpp
    store_format.cpp
    vocabulary_registry.cpp
    detail/errors.cpp
    detail/event_log.cpp
    detail/file_io.cpp
    detail/geometry_codec.cpp
    detail/half_float.cpp
    detail/label_aliases.cpp
    detail/measurement_names.cpp
    detail/object_lookup.cpp
    detail/polygon_predicates.cpp
    detail/pyramid_tiff_writer.cpp
    detail/query_builder.cpp
    detail/raster_files.cpp
    detail/schema.cpp
    detail/sqlite.cpp
    detail/store_impl.cpp
    detail/tiff_io.cpp
    detail/tile_spool.cpp
    detail/writer_lock.cpp
)
# The wake-up signal (ruling S13): a named event on Windows; elsewhere a POSIX named
# semaphore (Linux) or no signal at all (macOS), both in the POSIX source.
if(WIN32)
    target_sources(slideio-processor-storage PRIVATE detail/change_signal_win32.cpp)
else()
    target_sources(slideio-processor-storage PRIVATE detail/change_signal_posix.cpp)
endif()
# sem_* live in libpthread on glibc before 2.34.
target_link_libraries(slideio-processor-storage PRIVATE Threads::Threads)
set_target_properties(slideio-processor-storage PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_definitions(slideio-processor-storage PRIVATE SLIDEIO_PROCESSOR_DATA_API)
# SQLite is linked PRIVATE and never exported from the library.
target_link_libraries(slideio-processor-storage PRIVATE SQLite::SQLite3 rapidjson)
# libtiff is linked PRIVATE and never exported either (ruling S40).
target_link_libraries(slideio-processor-storage PRIVATE TIFF::TIFF ZLIB::ZLIB)
target_include_directories(slideio-processor-storage
    PUBLIC ${PROJECT_SOURCE_DIR}/include
    PRIVATE ${PROJECT_SOURCE_DIR}/src)
