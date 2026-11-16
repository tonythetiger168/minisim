# Expression simplification in msim -compile (src/exprsimp.hpp): random
# designs made of identity shapes (tests/fuzz/gen_simp.py), with x and z
# operand values, must print exactly what the interpreter prints, and the
# rewriter must actually fire. Called with -DMSIM -DSRC -DOUT -DPY (python3).
foreach(seed 1 2 3 4)
  set(sv ${OUT}/simp_${seed}.sv)
  execute_process(COMMAND ${PY} ${SRC}/tests/fuzz/gen_simp.py ${seed} OUTPUT_FILE ${sv} RESULT_VARIABLE r)
  if(NOT r EQUAL 0)
    message(FATAL_ERROR "gen_simp.py ${seed} failed")
  endif()
  execute_process(COMMAND ${MSIM} -threads 1 ${sv}
                  OUTPUT_VARIABLE want RESULT_VARIABLE r1 ERROR_VARIABLE err1)
  execute_process(COMMAND ${CMAKE_COMMAND} -E env MSIM_WHYSIMP=1 ${MSIM} -compile -O1 -o ${OUT}/sp${seed} ${sv}
                  OUTPUT_VARIABLE got RESULT_VARIABLE r2 ERROR_VARIABLE err)
  if(NOT r1 EQUAL 0 OR NOT r2 EQUAL 0 OR err MATCHES "using the interpreter")
    message(FATAL_ERROR "seed ${seed}: exit ${r1} / ${r2}\n${err1}\n${err}")
  endif()
  if(NOT want MATCHES "23 ")
    message(FATAL_ERROR "seed ${seed}: the design did not run to its last step\n${want}")
  endif()
  if(NOT err MATCHES "simplified: [1-9]")
    message(FATAL_ERROR "seed ${seed}: no rewrites\n${err}")
  endif()
  if(NOT want STREQUAL got)
    message(FATAL_ERROR "seed ${seed}: -compile differs\n--- interpreter\n${want}--- compiled\n${got}")
  endif()
endforeach()
message(STATUS "4 simplification designs match")
