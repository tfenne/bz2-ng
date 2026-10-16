# Requires the shared library LIB to export exactly the functions in EXPECTED
# (a ;-list) and nothing else.  A Windows DLL is read with DUMPBIN (MSVC's
# dumpbin) or OBJDUMP (GNU objdump for the target), which list the export
# table's names.
# usage: cmake -DLIB=... -DEXPECTED=... [-DDUMPBIN=... | -DOBJDUMP=...] -P exported_symbols.cmake
# objdump translates its headings ("[Ordinal/Name Pointer] Table") into the
# user's language.
set(ENV{LC_ALL} C)
if(LIB MATCHES "[.]dll$" AND DUMPBIN)
  execute_process(COMMAND ${DUMPBIN} /nologo /exports ${LIB} OUTPUT_VARIABLE _out RESULT_VARIABLE _rc)
elseif(LIB MATCHES "[.]dll$")
  if(NOT OBJDUMP)
    message(FATAL_ERROR "a DLL needs DUMPBIN (MSVC's dumpbin) or OBJDUMP, GNU objdump for its target")
  endif()
  execute_process(COMMAND ${OBJDUMP} -p ${LIB} OUTPUT_VARIABLE _out RESULT_VARIABLE _rc)
elseif(APPLE)
  execute_process(COMMAND nm -gU ${LIB} OUTPUT_VARIABLE _out RESULT_VARIABLE _rc)
else()
  execute_process(COMMAND nm -D --defined-only ${LIB} OUTPUT_VARIABLE _out RESULT_VARIABLE _rc)
endif()
if(NOT _rc EQUAL 0)
  message(FATAL_ERROR "listing the symbols of ${LIB} failed")
endif()
string(REPLACE "\r" "" _out "${_out}")
string(REPLACE "\n" ";" _lines "${_out}")
set(_got "")
set(_in_export_names 0)
foreach(_line IN LISTS _lines)
  if(LIB MATCHES "[.]dll$" AND DUMPBIN)
    # One "ordinal hint RVA name" line per export.  Names are kept whole, as
    # below.
    if(_line MATCHES "^[ \t]+[0-9]+[ \t]+[0-9A-Fa-f]+[ \t]+[0-9A-Fa-f]+[ \t]+([A-Za-z0-9_@?$]+)")
      list(APPEND _got ${CMAKE_MATCH_1})
    endif()
  elseif(LIB MATCHES "[.]dll$")
    # The names follow "[Ordinal/Name Pointer] Table", one "[ n] name" each;
    # binutils 2.44 and later add "+base[ m]  hint" before the name and a line
    # of column names after the heading.  The names are kept whole: a 32-bit
    # stdcall name decorated with "@n" is not the name libbz2's own DLL exports.
    if(_line MATCHES "^\\[Ordinal/Name Pointer\\] Table")
      set(_in_export_names 1)
    elseif(_in_export_names AND _line MATCHES "^[ \t]*Ordinal[ \t]+Hint[ \t]+Name$")
      # the columns' names
    elseif(_in_export_names AND _line MATCHES "^[ \t]*\\[ *[0-9]+\\]( \\+base\\[ *[0-9]+\\] +[0-9A-Fa-f]+)? ([A-Za-z0-9_@]+)$")
      list(APPEND _got ${CMAKE_MATCH_2})
    elseif(_in_export_names)
      set(_in_export_names 0)
    endif()
  elseif(_line MATCHES "^[0-9a-fA-F]+ [A-Za-z] _?([A-Za-z0-9_@.]+)$")
    set(_sym ${CMAKE_MATCH_1})
    string(REGEX REPLACE "@.*" "" _sym "${_sym}")
    list(APPEND _got ${_sym})
  endif()
endforeach()
list(SORT _got)
list(REMOVE_DUPLICATES _got)
set(_want ${EXPECTED})
list(SORT _want)
if(NOT _got STREQUAL _want)
  message(FATAL_ERROR "exported symbols differ.\n got: ${_got}\nwant: ${_want}")
endif()
list(LENGTH _got _n)
message(STATUS "${_n} symbols exported, as expected")
