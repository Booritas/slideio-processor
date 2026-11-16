# Fails when an MSVC build configuration does not match the SlideIO it links.
# SlideIO's API passes std::string, std::vector and std::shared_ptr across its
# DLL boundary, and MSVC lays those out differently in Debug (/MDd, iterator
# debugging) and in the other configurations, with nothing to detect a mismatch
# at link or load time. SlideIO's Debug libraries carry a _d suffix.
# Usage: cmake -DCONFIG=<configuration> -DSLIDEIO_LIBRARY=<import library>
#              -P check_slideio_configuration.cmake
# Also include()d at configure time with the same two variables set.
if(NOT SLIDEIO_LIBRARY)
    message(FATAL_ERROR "SLIDEIO_LIBRARY is not set")
endif()

get_filename_component(_slideio_stem "${SLIDEIO_LIBRARY}" NAME_WE)
if(_slideio_stem MATCHES "_d$")
    set(_slideio_is_debug TRUE)
else()
    set(_slideio_is_debug FALSE)
endif()
if(CONFIG STREQUAL "Debug")
    set(_config_is_debug TRUE)
else()
    set(_config_is_debug FALSE)
endif()

if(_config_is_debug AND NOT _slideio_is_debug)
    message(FATAL_ERROR
        "An MSVC Debug build cannot link the Release SlideIO ${SLIDEIO_LIBRARY}: "
        "the STL types in SlideIO's API differ in layout between Debug and Release. "
        "Build Release (./build.sh), or install SlideIO in Debug and point "
        "CMAKE_PREFIX_PATH at that install.")
elseif(CONFIG AND NOT _config_is_debug AND _slideio_is_debug)
    message(FATAL_ERROR
        "An MSVC ${CONFIG} build cannot link the Debug SlideIO ${SLIDEIO_LIBRARY}: "
        "the STL types in SlideIO's API differ in layout between Debug and Release. "
        "Point CMAKE_PREFIX_PATH at a Release SlideIO install.")
endif()
