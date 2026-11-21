cmake_minimum_required(VERSION 3.21)
project(slideio-processor VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

# One output directory, so test executables find the shared library.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8 /wd4251 /wd4275)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# The engine links SlideIO, whose API carries STL types, so an MSVC build must use a
# SlideIO installed in the same configuration (checked in src/CMakeLists.txt).
find_package(slideio 2.10 REQUIRED)
# The data library uses OpenCV for its version only; the engine links it statically
# and privately (see src/CMakeLists.txt).
# No version in find_package: the Conan-generated OpenCVConfigVersion.cmake accepts
# only an exact major match, which would reject OpenCV 5 (UR-BLD-4).
find_package(OpenCV REQUIRED)
if(OpenCV_VERSION VERSION_LESS 4)
  message(FATAL_ERROR "OpenCV 4 or later is required; found ${OpenCV_VERSION}")
endif()
find_package(RapidJSON REQUIRED)
find_package(SQLite3 REQUIRED)
# Raster TIFF I/O in storage, linked privately (see src/storage/CMakeLists.txt).
find_package(TIFF REQUIRED)
find_package(ZLIB REQUIRED)
# The storage wake-up signal uses sem_*, in libpthread on glibc before 2.34.
find_package(Threads REQUIRED)

# Test seams: test-only exports (the detail::*ForTesting functions and
# detail::trackForCloseTesting) and the production branches that read them. Turn this off
# for a release package: the seams are compiled out, GTest is not required and tests/ is
# not built; scripts/check-no-test-seams.sh checks the result. Not gated, as they are not
# exported: WriterLock::acquire(path, afterOpen), TiffRasterReader's openHook_ and
# TileSpool::piecesStored/piecesDecoded.
option(SLIDEIO_PROCESSOR_TEST_SEAMS
       "Build and export the test-only seams the tests need; turn off for release packages" ON)
if(SLIDEIO_PROCESSOR_TEST_SEAMS)
    # Directory-scoped: nothing reaches an installed or exported package configuration.
    add_compile_definitions(SLIDEIO_PROCESSOR_TEST_SEAMS=1)
    find_package(GTest REQUIRED)
else()
    message(STATUS "slideio-processor: test seams off; tests are not built")
endif()

enable_testing()
add_subdirectory(src)
if(SLIDEIO_PROCESSOR_TEST_SEAMS)
    add_subdirectory(tests)
endif()

# Optional API documentation: only when Doxygen is installed.
find_package(Doxygen QUIET)
if(DOXYGEN_FOUND)
    add_custom_target(docs
        COMMAND ${DOXYGEN_EXECUTABLE} ${PROJECT_SOURCE_DIR}/Doxyfile
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
        COMMENT "Generating API documentation with Doxygen"
        VERBATIM)
endif()
