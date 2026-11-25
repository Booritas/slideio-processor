# A standalone consumer of the data library alone, as slideio-view uses it: the library
# comes in through add_subdirectory with SLIDEIO_PROCESSOR_DATA_ONLY, so neither SlideIO
# nor OpenCV is configured, and the consumer links slideio-processor-data only. Not part
# of the main build; scripts/check-data-only-consumer.sh configures, builds and runs it.
cmake_minimum_required(VERSION 3.21)
project(data_only_consumer CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(SLIDEIO_PROCESSOR_DATA_ONLY ON CACHE BOOL "" FORCE)
set(SLIDEIO_PROCESSOR_TEST_SEAMS OFF CACHE BOOL "" FORCE)

# The repository root, two levels up.
get_filename_component(PROCESSOR_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
add_subdirectory(${PROCESSOR_ROOT} slideio-processor)

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE slideio-processor-data)
