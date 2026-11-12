# msim -profile [<file>] (src/profile.hpp): the report names every process
# with its instance and source line, and the activation counts do not depend
# on timing: each counter's always_ff runs on the 100 rising edges.
#   cmake -DMSIM=<msim> -DSRC=<source dir> -DOUT=<dir> -P msim_profile.cmake
set(SV ${SRC}/tests/profile/counters.sv)
file(MAKE_DIRECTORY ${OUT})
file(REMOVE ${OUT}/simprofile.txt ${OUT}/p.txt)
foreach(args "-profile" "-profile;p.txt" "-compile;-profile")
  execute_process(COMMAND ${MSIM} ${SV} ${args} WORKING_DIRECTORY ${OUT}
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT rc EQUAL 0 OR NOT out STREQUAL "s=200\n")
    message(FATAL_ERROR "msim ${args} exited ${rc}: ${err}\n${out}")
  endif()
  if(args MATCHES "p.txt")
    set(rep ${OUT}/p.txt)
  else()
    set(rep ${OUT}/simprofile.txt)
  endif()
  file(READ ${rep} r)
  foreach(want
      "Simulation time: 1000 "
      "== Time by category ==\n"
      " +100  always_ff  top\\.u0  [^\n]*counters\\.sv:4\n"
      " +100  always_ff  top\\.u1  [^\n]*counters\\.sv:4\n"
      " +201  always  top  [^\n]*counters\\.sv:12\n"
      " +201  port clk  top\\.u0  -\n"
      "== Time by instance ==\n[^=]* 402  top\\.u0\n"
      "== Time by kind of process ==\n")
    if(NOT r MATCHES "${want}")
      message(FATAL_ERROR "msim ${args}: no match for '${want}' in\n${r}")
    endif()
  endforeach()
  file(REMOVE ${rep})
endforeach()
