# A waveform written by a compiled simulator (simv -vcd) holds the same values
# as the interpreter's: also for signals the compiled code keeps in private
# variables, reads at their source (port copies) or does not compute unless a
# waveform is open. Called with -DMSIM -DSRC -DOUT -DPY.
set(CASES "examples/deadcopy.sv" "examples/cycle.sv" "examples/combbranch.sv")
set(n 0)
foreach(c IN LISTS CASES)
  execute_process(COMMAND ${MSIM} -threads 1 -no-static-comb -no-static-seq -no-partition ${SRC}/${c} -vcd ${OUT}/v${n}_ref.vcd
                  OUTPUT_VARIABLE want RESULT_VARIABLE r1)
  execute_process(COMMAND ${MSIM} -compile -O1 -o ${OUT}/v${n} ${SRC}/${c} OUTPUT_VARIABLE got RESULT_VARIABLE r2 ERROR_VARIABLE err)
  if(NOT r1 EQUAL 0 OR NOT r2 EQUAL 0 OR NOT want STREQUAL got)
    message(FATAL_ERROR "${c}: exit ${r1} / ${r2}\n${err}\n--- interpreter\n${want}--- -compile\n${got}")
  endif()
  execute_process(COMMAND ${OUT}/v${n}/simv -vcd ${OUT}/v${n}.vcd OUTPUT_VARIABLE got2 RESULT_VARIABLE r3)
  if(NOT r3 EQUAL 0 OR NOT want STREQUAL got2)
    message(FATAL_ERROR "${c}: simv -vcd prints something else\n--- interpreter\n${want}--- simv -vcd\n${got2}")
  endif()
  execute_process(COMMAND ${PY} ${SRC}/tests/vcd_same.py ${OUT}/v${n}_ref.vcd ${OUT}/v${n}.vcd OUTPUT_VARIABLE diff RESULT_VARIABLE r4)
  if(NOT r4 EQUAL 0)
    message(FATAL_ERROR "${c}: the waveforms differ (time signal: interpreter / simv)\n${diff}")
  endif()
  message(STATUS "${c}: same waveform")
  math(EXPR n "${n} + 1")
endforeach()
