# Fails if a public header exposes an STL container, std::string, OpenCV or a template.
# Usage: cmake -DHEADER_DIR=<include dir> -P check_public_headers.cmake
if(NOT HEADER_DIR)
    message(FATAL_ERROR "HEADER_DIR is not set")
endif()

file(GLOB_RECURSE headers ${HEADER_DIR}/*.hpp)
set(forbidden_include
    "^[ \t]*#[ \t]*include[ \t]*<(vector|list|map|set|unordered_map|unordered_set|deque|array|string|string_view)>"
    "^[ \t]*#[ \t]*include[ \t]*<opencv2/")
set(forbidden_template "template[ \t]*<")

set(count 0)
set(failed FALSE)
foreach(header ${headers})
    math(EXPR count "${count} + 1")
    file(STRINGS ${header} lines)
    set(line_no 0)
    foreach(line IN LISTS lines)
        math(EXPR line_no "${line_no} + 1")
        foreach(pattern ${forbidden_include} ${forbidden_template})
            if(line MATCHES "${pattern}")
                message(SEND_ERROR "${header}:${line_no}: forbidden in a public header: ${line}")
                set(failed TRUE)
            endif()
        endforeach()
    endforeach()
endforeach()

if(failed)
    message(FATAL_ERROR "public header check failed")
endif()
message(STATUS "checked ${count} headers")
