# Random expressions (tests/fuzz/gen_expr.py): msim -compile, with its 2-state
# fast path, must print what the 4-state interpreter prints, values with x
# and z included. Called with -DMSIM -DSRC -DOUT -DPY (python3).
foreach(seed 1 2 3)
  set(sv ${OUT}/expr_${seed}.sv)
  execute_process(COMMAND ${PY} ${SRC}/tests/fuzz/gen_expr.py ${seed} OUTPUT_FILE ${sv} RESULT_VARIABLE r)
  if(NOT r EQUAL 0)
    message(FATAL_ERROR "gen_expr.py ${seed} failed")
  endif()
  execute_process(COMMAND ${MSIM} -threads 1 -no-static-comb -no-static-seq -no-partition ${sv}
                  OUTPUT_VARIABLE want RESULT_VARIABLE r1 ERROR_QUIET)
  execute_process(COMMAND ${MSIM} -compile -O1 -o ${OUT}/fz${seed} ${sv}
                  OUTPUT_VARIABLE got RESULT_VARIABLE r2 ERROR_VARIABLE err)
  if(NOT r1 EQUAL 0 OR NOT r2 EQUAL 0 OR err MATCHES "using the interpreter")
    message(FATAL_ERROR "seed ${seed}: exit ${r1} / ${r2}\n${err}")
  endif()
  if(NOT want MATCHES "23 ")
    message(FATAL_ERROR "seed ${seed}: the design did not run to its last step\n${want}")
  endif()
  if(NOT want STREQUAL got)
    message(FATAL_ERROR "seed ${seed}: -compile differs\n--- interpreter\n${want}--- compiled\n${got}")
  endif()
endforeach()
message(STATUS "3 random designs match")
