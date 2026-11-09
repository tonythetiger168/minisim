# Run one spec test: simulate SV with SIM and compare stdout with the file's
# expected lines. An expected line is a comment that starts with "//> ".
#   cmake -DSIM=<msim|iverilog> -DSV=<file.sv> [-DVVP=<vvp>] -P run_spec.cmake
# "//! args: <words>" adds msim command-line arguments (more source files,
# -libmap, -sdf ...), relative to the test's directory.
# With -DVVP set, SIM is iverilog: compile with -g2012, then run under vvp and
# drop vvp's own "$finish called" / "VCD info" lines.
# Each test runs in its own directory, so a data file is a relative path.
get_filename_component(dir ${SV} DIRECTORY)
get_filename_component(base ${SV} NAME)   # run as a relative name, so `__FILE__ and messages are stable
file(STRINGS ${SV} lines)
set(expected "")
set(args "")
set(want_rc 0)   # "//! exit: <n>": the exit code msim should return (e.g. 1 after $fatal)
foreach(l IN LISTS lines)
  if(l MATCHES "^//> (.*)$")
    string(APPEND expected "${CMAKE_MATCH_1}\n")
  elseif(l STREQUAL "//>")
    string(APPEND expected "\n")
  elseif(l MATCHES "^//! exit: ([0-9]+)")
    set(want_rc ${CMAKE_MATCH_1})
  elseif(l MATCHES "^//! args: (.*)$")
    separate_arguments(extra UNIX_COMMAND "${CMAKE_MATCH_1}")
    list(APPEND args ${extra})
  endif()
endforeach()

if(VVP)
  get_filename_component(name ${SV} NAME)  # NAME_WE would cut 6.7.1-x at "6"
  set(out ${CMAKE_CURRENT_BINARY_DIR}/icarus_${name}.vvp)
  execute_process(COMMAND ${SIM} -g2012 -o ${out} ${SV}
                  RESULT_VARIABLE rc ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "iverilog failed on ${SV}:\n${err}")
  endif()
  execute_process(COMMAND ${VVP} -n ${out} OUTPUT_VARIABLE got RESULT_VARIABLE rc TIMEOUT 20
                  WORKING_DIRECTORY ${dir})
  string(REGEX REPLACE "[^\n]*\\$finish called[^\n]*\n" "" got "${got}")
  string(REGEX REPLACE "VCD info:[^\n]*\n" "" got "${got}")
  string(REGEX REPLACE "WARNING: [^\n]*\n" "" got "${got}")   # vvp's warnings go to stdout
else()
  execute_process(COMMAND ${SIM} ${base} ${args} OUTPUT_VARIABLE got ERROR_VARIABLE err
                  RESULT_VARIABLE rc TIMEOUT 20 WORKING_DIRECTORY ${dir})
  if(NOT rc STREQUAL want_rc)
    message(FATAL_ERROR "msim exited ${rc} (expected ${want_rc}) on ${SV}:\n${err}")
  endif()
endif()

if(NOT got STREQUAL expected)
  message(FATAL_ERROR "output differs for ${SV}\n--- expected\n${expected}--- got\n${got}")
endif()
