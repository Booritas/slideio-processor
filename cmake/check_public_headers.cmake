# Fails if a public header breaks the ABI rules (SDS 00 C-2):
# - includes an STL container, std::string, OpenCV, or one of <functional>,
#   <optional>, <variant>, <tuple>, <utility>;
# - declares a template;
# - names, outside comments, any std:: entity other than shared_ptr,
#   unique_ptr, exception, nullptr_t, int*_t and uint*_t.
# Each violation is reported as <file>:<line>: <reason>: <line text>.
# Usage: cmake -DHEADER_DIR=<include dir> -P check_public_headers.cmake
if(NOT HEADER_DIR)
    message(FATAL_ERROR "HEADER_DIR is not set")
endif()

file(GLOB_RECURSE headers ${HEADER_DIR}/*.hpp)
set(forbidden_include
    "^[ \t]*#[ \t]*include[ \t]*<(vector|list|map|set|unordered_map|unordered_set|deque|array|string|string_view|functional|optional|variant|tuple|utility)>"
    "^[ \t]*#[ \t]*include[ \t]*<opencv2/")
set(forbidden_template "template[ \t]*<")
set(allowed_std "^(shared_ptr|unique_ptr|exception|nullptr_t|u?int(8|16|32|64)_t)$")

set(count 0)
set(failed FALSE)
foreach(header ${headers})
    math(EXPR count "${count} + 1")
    # Split into lines by hand: file(STRINGS) would split a line at every ';'.
    # Semicolons and brackets are swapped for placeholders so that list
    # handling leaves each line whole; they are restored for messages.
    file(READ ${header} content)
    string(REPLACE "\r" "" content "${content}")
    string(REPLACE ";" "<SEMI>" content "${content}")
    string(REPLACE "[" "<LB>" content "${content}")
    string(REPLACE "]" "<RB>" content "${content}")
    string(REPLACE "\n" ";" lines "${content}")
    set(line_no 0)
    set(in_block FALSE)
    foreach(raw IN LISTS lines)
        math(EXPR line_no "${line_no} + 1")
        string(REPLACE "<SEMI>" ";" text "${raw}")
        string(REPLACE "<LB>" "[" text "${text}")
        string(REPLACE "<RB>" "]" text "${text}")

        # Code: the line with /* ... */ and // comments removed.
        set(rest "${text}")
        set(code "")
        while(NOT rest STREQUAL "")
            if(in_block)
                string(FIND "${rest}" "*/" close)
                if(close EQUAL -1)
                    set(rest "")
                else()
                    math(EXPR after "${close} + 2")
                    string(SUBSTRING "${rest}" ${after} -1 rest)
                    set(in_block FALSE)
                endif()
            else()
                string(FIND "${rest}" "/*" open)
                string(FIND "${rest}" "//" line_comment)
                if(NOT line_comment EQUAL -1 AND (open EQUAL -1 OR line_comment LESS open))
                    string(SUBSTRING "${rest}" 0 ${line_comment} piece)
                    string(APPEND code "${piece}")
                    set(rest "")
                elseif(open EQUAL -1)
                    string(APPEND code "${rest}")
                    set(rest "")
                else()
                    string(SUBSTRING "${rest}" 0 ${open} piece)
                    string(APPEND code "${piece} ")
                    math(EXPR after "${open} + 2")
                    string(SUBSTRING "${rest}" ${after} -1 rest)
                    set(in_block TRUE)
                endif()
            endif()
        endwhile()

        foreach(pattern ${forbidden_include})
            if(code MATCHES "${pattern}")
                message(SEND_ERROR "${header}:${line_no}: forbidden include in a public header: ${text}")
                set(failed TRUE)
            endif()
        endforeach()
        if(code MATCHES "${forbidden_template}")
            message(SEND_ERROR "${header}:${line_no}: template in a public header: ${text}")
            set(failed TRUE)
        endif()
        string(REGEX MATCHALL "std::[A-Za-z_][A-Za-z_0-9]*" std_names "${code}")
        foreach(qualified IN LISTS std_names)
            string(SUBSTRING "${qualified}" 5 -1 name)
            if(NOT name MATCHES "${allowed_std}")
                message(SEND_ERROR "${header}:${line_no}: std::${name} in a public header: ${text}")
                set(failed TRUE)
            endif()
        endforeach()
    endforeach()
endforeach()

if(failed)
    message(FATAL_ERROR "public header check failed")
endif()
message(STATUS "checked ${count} headers")
