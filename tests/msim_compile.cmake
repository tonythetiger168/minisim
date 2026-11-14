# Every execution mode must print the same. The reference is the interpreter
# with only event-driven processes, on one thread. The default interpreter
# (static schedule, clocked functions, partitions, every hardware thread),
# the same on one thread, and msim -compile must match it.
# -O1 keeps the C++ builds quick. Called with -DMSIM=<msim> -DSRC=<source dir> -DOUT=<scratch dir>,
# and -DSHARD=<k> -DSHARDS=<n> to run only every n-th case from the k-th
# (CMakeLists.txt registers msim_compile_0 .. _<n-1>: each build compiles
# the kernel, about 20 s, so the cases run in parallel test processes).
if(NOT DEFINED SHARDS)
  set(SHARDS 1)
  set(SHARD 0)
endif()
set(ran 0)
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
  "examples/seqtmpidx.sv +define+LFSR"
  "examples/atstarpriv.sv"
  "examples/emptycall.sv"
  "examples/privtmp.sv"
  "examples/wideslice.sv"
  "examples/nbadelay.sv"
  "-cfiles 3 examples/atstarpriv.sv"
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
  math(EXPR mine "${n} % ${SHARDS}")
  if(NOT mine EQUAL SHARD)
    math(EXPR n "${n} + 1")
    continue()
  endif()
  math(EXPR ran "${ran} + 1")
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
# Power-aware simulation (msim -upf, IEEE 1801-2024): simv runs the same
# power intent from msim's snapshot (upf_rt.inc). Domains switched off and
# on, isolation, corruption, retention of flops and latches, isolation cells
# by their functional model (HDL ports on supply nets), replayed initial
# blocks, a process whose domain powers down in the middle of its body, and
# the low-power coverage report.
foreach(t gating retlatch isomodel replay restart)
  math(EXPR mine "${n} % ${SHARDS}")
  if(NOT mine EQUAL SHARD)
    math(EXPR n "${n} + 1")
    continue()
  endif()
  math(EXPR ran "${ran} + 1")
  set(files ${SRC}/tests/upf/${t}.sv -upf ${SRC}/tests/upf/${t}.upf)
  execute_process(COMMAND ${MSIM} ${files} OUTPUT_VARIABLE want RESULT_VARIABLE r1)
  execute_process(COMMAND ${MSIM} -compile -O1 -o ${OUT}/upf_${t} ${files}
                  OUTPUT_VARIABLE got RESULT_VARIABLE r2 ERROR_VARIABLE err)
  if(NOT r1 EQUAL 0 OR NOT r2 EQUAL 0)
    message(FATAL_ERROR "upf ${t}: exit ${r1} (interpreter) / ${r2} (-compile)\n${err}")
  endif()
  if(err MATCHES "using the interpreter")
    message(FATAL_ERROR "upf ${t}: -compile fell back to the interpreter\n${err}")
  endif()
  if(NOT want STREQUAL got)
    message(FATAL_ERROR "upf ${t}: -compile output differs\n--- interpreter\n${want}--- compiled\n${got}")
  endif()
  math(EXPR n "${n} + 1")
endforeach()
math(EXPR mine "${n} % ${SHARDS}")
if(mine EQUAL SHARD)
  execute_process(COMMAND ${MSIM} -compile -O1 -o ${OUT}/upf_lpcov ${SRC}/tests/upf/retlatch.sv -upf ${SRC}/tests/upf/lpcov.upf
                          -upf_cov ${OUT}/lpcov.cov
                  RESULT_VARIABLE rc ERROR_VARIABLE err)
  file(READ ${SRC}/tests/upf/lpcov.cov want)
  file(READ ${OUT}/lpcov.cov got)
  if(NOT rc EQUAL 0 OR NOT want STREQUAL got)
    message(FATAL_ERROR "upf lpcov: -compile coverage differs: ${rc}\n${err}--- got\n${got}--- want\n${want}")
  endif()
  math(EXPR ran "${ran} + 1")
endif()
# $fatal: the same exit status and output from the compiled simv.
math(EXPR n "${n} + 1")
math(EXPR mine "${n} % ${SHARDS}")
if(mine EQUAL SHARD)
  execute_process(COMMAND ${MSIM} -threads 1 ${SRC}/examples/fatalexit.sv OUTPUT_VARIABLE want RESULT_VARIABLE r1 ERROR_VARIABLE e1)
  execute_process(COMMAND ${MSIM} -compile -O1 -o ${OUT}/fatalexit ${SRC}/examples/fatalexit.sv
                  OUTPUT_VARIABLE got RESULT_VARIABLE r2 ERROR_VARIABLE err)
  if(err MATCHES "using the interpreter")
    message(FATAL_ERROR "fatalexit: -compile fell back to the interpreter\n${err}")
  endif()
  if(NOT r1 EQUAL 1 OR NOT r2 EQUAL 1 OR NOT want STREQUAL got)
    message(FATAL_ERROR "fatalexit: exit ${r1} / ${r2} (want 1)\n--- interpreter\n${want}--- compiled\n${got}")
  endif()
  math(EXPR ran "${ran} + 1")
endif()
message(STATUS "${ran} of ${n} designs match (shard ${SHARD} of ${SHARDS})")
