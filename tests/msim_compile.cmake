# Every execution mode must print the same. The reference is the interpreter
# with only event-driven processes, on one thread. The default interpreter
# (static schedule, clocked functions, partitions, every hardware thread),
# the same on one thread, and msim -compile must match it.
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
  "examples/cycle.sv"
  "examples/memories.sv"
  "examples/assertions.sv"
  "-cfiles 3 examples/memories.sv"
  "-cfiles 3 examples/assertions.sv"
  "-cfiles 3 examples/islands.sv"
  "-cfiles 3 examples/deadcopy.sv"
  "-cfiles 3 examples/widesplit.sv"
  "-cfiles 3 examples/timeps.sv"
  "examples/loopreset.sv"
  "-cfiles 3 examples/loopreset.sv"
  "examples/seqtemp.sv"
  "-cfiles 3 examples/seqtemp.sv"
  "examples/shareinst.sv"
  "examples/manygroups.sv"
  "examples/islands.sv"
  "examples/combbranch.sv"
  "examples/combcycle.sv"
  "examples/combsplit.sv"
  "examples/combbidx.sv"
  "examples/clkcone.sv"
  "examples/combcall.sv"
  "examples/rstcopy.sv"
  "examples/deadcopy.sv"
  "examples/widesplit.sv"
  "examples/timeps.sv"
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
  execute_process(COMMAND ${MSIM} -threads 1 -no-static-comb -no-static-seq -no-partition ${files}
                  OUTPUT_VARIABLE want RESULT_VARIABLE r1)
  execute_process(COMMAND ${MSIM} -threads 1 ${files} OUTPUT_VARIABLE one RESULT_VARIABLE r4)
  if(NOT r4 EQUAL 0 OR NOT want STREQUAL one)
    message(FATAL_ERROR "${c}: -threads 1 differs\n--- event-driven\n${want}--- -threads 1\n${one}")
  endif()
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
  # The benchmark's registers and wires must stay plain variables of the
  # cycle domain (codegen.hpp privateSigs): without them it runs 10x slower
  # and prints the same.
  if(c MATCHES "^bench/lanes")
    file(READ ${OUT}/c${n}/design.cpp gen)
    string(REGEX MATCHALL "PSig&? P[0-9]+" hits "${gen}")
    list(LENGTH hits nh)
    if(nh LESS 48)
      message(FATAL_ERROR "${c}: only ${nh} signals private to the cycle domain (expected 48)")
    endif()
  endif()
  # Instances of one module share one copy of its code (codegen.hpp
  # shareChunks, emitDomainFns).
  if(c STREQUAL "examples/shareinst.sv")
    file(READ ${OUT}/c${n}/design.cpp gen)
    if(NOT gen MATCHES "dsT[0-9]+\\(" OR NOT gen MATCHES "combPT[0-9]+_0\\(")
      message(FATAL_ERROR "${c}: the instances do not share their code")
    endif()
  endif()
  math(EXPR n "${n} + 1")
endforeach()
message(STATUS "${n} designs match")
