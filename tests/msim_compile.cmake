# Every execution mode must print the same: the interpreter with ordinary
# event-driven processes (-no-static-comb) is the reference; the interpreter
# with the static combinational schedule (default) and msim -compile must match.
# -O1 keeps the twelve C++ builds quick. Called with -DMSIM=<msim> -DSRC=<source dir> -DOUT=<scratch dir>.
set(CASES
  "examples/compound.sv"
  "examples/div.sv"
  "examples/signed.sv"
  "examples/strengths.sv"
  "examples/tasks.sv"
  "examples/breakcont.sv"
  "examples/hierarchy.sv"
  "examples/checks.sv"
  "examples/comb.sv"
  "bench/chain.sv +define+END_TIME=2000"
  "examples/cli/init.sv"
  "examples/cli/adder.sv examples/cli/adder_tb.sv -top adder_tb +define+WIDTH=8"
  "examples/cli/spin.sv -maxtime 25"
  "bench/lanes.sv +define+END_TIME=2000")
set(n 0)
foreach(c IN LISTS CASES)
  separate_arguments(args UNIX_COMMAND "${c}")
  set(files "")
  foreach(a IN LISTS args)
    if(a MATCHES "\\.sv$")
      list(APPEND files "${SRC}/${a}")
    else()
      list(APPEND files "${a}")
    endif()
  endforeach()
  execute_process(COMMAND ${MSIM} -no-static-comb ${files} OUTPUT_VARIABLE want RESULT_VARIABLE r1)
  execute_process(COMMAND ${MSIM} ${files} OUTPUT_VARIABLE lev RESULT_VARIABLE r3)
  if(NOT r3 EQUAL 0 OR NOT want STREQUAL lev)
    message(FATAL_ERROR "${c}: static combinational schedule differs\n--- event-driven\n${want}--- levelized\n${lev}")
  endif()
  execute_process(COMMAND ${MSIM} -compile -O1 -o ${OUT}/c${n} ${files}
                  OUTPUT_VARIABLE got RESULT_VARIABLE r2 ERROR_VARIABLE err)
  if(NOT r1 EQUAL 0 OR NOT r2 EQUAL 0)
    message(FATAL_ERROR "${c}: exit ${r1} (interpreter) / ${r2} (-compile)\n${err}")
  endif()
  if(err MATCHES "using the interpreter")
    message(FATAL_ERROR "${c}: -compile fell back to the interpreter\n${err}")
  endif()
  if(NOT want STREQUAL got)
    message(FATAL_ERROR "${c}: -compile output differs\n--- interpreter\n${want}--- compiled\n${got}")
  endif()
  math(EXPR n "${n} + 1")
endforeach()
message(STATUS "${n} designs match")
