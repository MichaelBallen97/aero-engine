# cmake/reflect_declarations_join.cmake -- task 4.2.2. Run by aero_reflect_generate_declarations' custom command as
# `cmake -P`: joins the files PARTS lists (one absolute path per line, the prelude first, then one --emit-dts
# fragment per header) into OUTPUT. -DPARTS=<manifest> -DOUTPUT=<file>.
cmake_minimum_required(VERSION 3.28)

foreach(variable IN ITEMS PARTS OUTPUT)
    if(NOT DEFINED ${variable} OR "${${variable}}" STREQUAL "")
        message(FATAL_ERROR "declarations: ${variable} is required")
    endif()
endforeach()
file(STRINGS "${PARTS}" parts)
if(NOT parts)
    message(FATAL_ERROR "declarations: '${PARTS}' names no part")
endif()
set(AERO_DECLARATIONS_TEXT "")
foreach(part IN LISTS parts)
    if(NOT EXISTS "${part}")
        message(FATAL_ERROR "declarations: part '${part}' does not exist")
    endif()
    # A TEXT read: on Windows the C runtime turns the CRLF a text-mode -o wrote back into LF here.
    file(READ "${part}" text)
    if(text STREQUAL "")
        message(FATAL_ERROR "declarations: part '${part}' is empty")
    endif()
    # Printable ASCII and LF only, and never '@': the write below is a configure substitution, and the editor
    # embeds this file as 7-bit text. cmake/embed_run.cmake's text mode re-checks every byte.
    string(REGEX MATCH "[^\n -?A-~]" bad "${text}")
    if(NOT bad STREQUAL "")
        message(FATAL_ERROR "declarations: part '${part}' holds a byte outside printable ASCII and LF, or an '@'")
    endif()
    if(NOT text MATCHES "\n$")
        string(APPEND text "\n")
    endif()
    string(APPEND AERO_DECLARATIONS_TEXT "${text}")
endforeach()
# LF on every OS, and written only when the text changed, so an unchanged join recompiles nothing downstream.
# file(CONFIGURE) writes one LF after the last line whenever NEWLINE_STYLE is given (even when the content already
# ends in one), so the last LF is dropped here: the output is then exactly the parts, concatenated.
string(REGEX REPLACE "\n$" "" AERO_DECLARATIONS_TEXT "${AERO_DECLARATIONS_TEXT}")
file(CONFIGURE OUTPUT "${OUTPUT}" CONTENT "@AERO_DECLARATIONS_TEXT@" @ONLY NEWLINE_STYLE LF)
