# Run one spec test: simulate SV with SIM and compare stdout with the file's
# expected lines. An expected line is a comment that starts with "//> ".
#   cmake -DSIM=<msim|iverilog> -DSV=<file.sv> [-DVVP=<vvp>] -P run_spec.cmake
# With -DVVP set, SIM is iverilog: compile with -g2012, then run under vvp and
# drop vvp's own "$finish called" / "VCD info" lines.
file(STRINGS ${SV} lines)
set(expected "")
foreach(l IN LISTS lines)
  if(l MATCHES "^//> (.*)$")
    string(APPEND expected "${CMAKE_MATCH_1}\n")
  elseif(l STREQUAL "//>")
    string(APPEND expected "\n")
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
  execute_process(COMMAND ${VVP} -n ${out} OUTPUT_VARIABLE got RESULT_VARIABLE rc TIMEOUT 20)
  string(REGEX REPLACE "[^\n]*\\$finish called[^\n]*\n" "" got "${got}")
  string(REGEX REPLACE "VCD info:[^\n]*\n" "" got "${got}")
else()
  execute_process(COMMAND ${SIM} ${SV} OUTPUT_VARIABLE got ERROR_VARIABLE err
                  RESULT_VARIABLE rc TIMEOUT 20)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "msim exited ${rc} on ${SV}:\n${err}")
  endif()
endif()

if(NOT got STREQUAL expected)
  message(FATAL_ERROR "output differs for ${SV}\n--- expected\n${expected}--- got\n${got}")
endif()
