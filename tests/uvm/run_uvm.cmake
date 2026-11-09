# Run one UVM test against the Accellera UVM library and check its output.
#   cmake -DMSIM=<msim> -DUVM=<uvm-core>/src -DSV=<test.sv> -P run_uvm.cmake
# Each "//> " line of the test must appear in the output, in order, as part
# of a line. A "//! args: ..." line adds simulator arguments (plusargs).
# The library is built without its C code (+define+UVM_NO_DPI).
get_filename_component(dir ${SV} DIRECTORY)
get_filename_component(base ${SV} NAME)
file(STRINGS ${SV} lines)
set(expected "")
set(args "")
foreach(l IN LISTS lines)
  if(l MATCHES "^//> (.*)$")
    list(APPEND expected "${CMAKE_MATCH_1}")
  elseif(l MATCHES "^//! args: (.*)$")
    separate_arguments(args UNIX_COMMAND "${CMAKE_MATCH_1}")
  endif()
endforeach()
execute_process(COMMAND ${MSIM} +incdir+${UVM} ${UVM}/uvm_pkg.sv ${base} -top top +define+UVM_NO_DPI ${args}
                OUTPUT_VARIABLE got ERROR_VARIABLE err RESULT_VARIABLE rc TIMEOUT 600
                WORKING_DIRECTORY ${dir})
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "msim exited ${rc} on ${SV}:\n${err}\n${got}")
endif()
set(rest "${got}")
foreach(e IN LISTS expected)
  string(FIND "${rest}" "${e}" at)
  if(at EQUAL -1)
    message(FATAL_ERROR "missing (or out of order) in the output of ${SV}:\n  ${e}\n--- output ---\n${got}")
  endif()
  string(LENGTH "${e}" n)
  math(EXPR at "${at} + ${n}")
  string(SUBSTRING "${rest}" ${at} -1 rest)
endforeach()
