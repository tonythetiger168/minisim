# mregress self-test: run tests/regress/smoke.list on msim. Five runs pass and
# one (wrong_expect) must fail and one is a known gap (xfail), then -rerun repeats only that one.
#   cmake -DPY=<python3> -DMSIM=<msim> -DSRC=<source dir> -DOUT=<dir> -P mregress.cmake
set(MR ${PY} ${SRC}/tools/mregress.py)
file(REMOVE_RECURSE ${OUT})
execute_process(COMMAND ${MR} ${SRC}/tests/regress/smoke.list -exe ${MSIM} -o ${OUT} -seed 7 -j 2
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 1)
  message(FATAL_ERROR "mregress exited ${rc}, expected 1\n${out}${err}")
endif()
if(NOT out MATCHES "5 passed, 1 failed, 0 skipped, 1 known gaps")
  message(FATAL_ERROR "unexpected summary:\n${out}")
endif()
if(NOT out MATCHES "FAIL  wrong_expect  seed=7")
  message(FATAL_ERROR "wrong_expect did not fail:\n${out}")
endif()
foreach(f results.json junit.xml report.html runs/adder_seeds_s9/sim.log runs/adder_seeds_s9/cmd.sh)
  if(NOT EXISTS ${OUT}/${f})
    message(FATAL_ERROR "missing ${OUT}/${f}")
  endif()
endforeach()
file(READ ${OUT}/junit.xml junit)
if(NOT junit MATCHES "failures=\"1\"")
  message(FATAL_ERROR "junit.xml does not record one failure:\n${junit}")
endif()
execute_process(COMMAND ${MR} ${SRC}/tests/regress/smoke.list -exe ${MSIM} -o ${OUT} -rerun
                RESULT_VARIABLE rc OUTPUT_VARIABLE out)
if(NOT out MATCHES "FAIL  wrong_expect" OR out MATCHES "adder_seeds")
  message(FATAL_ERROR "-rerun should repeat only wrong_expect:\n${out}")
endif()
if(NOT out MATCHES "5 passed, 1 failed")
  message(FATAL_ERROR "-rerun lost the earlier passes:\n${out}")
endif()

# Spec mode: //! xfail: (known gap), an xfail that passes (xpass, an error),
# //! exit: <status>, and the per-chapter table.
execute_process(COMMAND ${MR} -spec ${SRC}/tests/regress/spec -exe ${MSIM} -o ${OUT}/spec -q -md ${OUT}/spec/chapters.md
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 1 OR NOT out MATCHES "2 passed, 1 failed, 0 skipped, 1 known gaps")
  message(FATAL_ERROR "spec mode: rc=${rc}\n${out}${err}")
endif()
if(NOT out MATCHES "\\| 2 \\| 1 \\| 1 \\| 0 \\| 2-xpass \\(xfail passes\\) \\| 50% \\|")
  message(FATAL_ERROR "spec mode chapter table:\n${out}")
endif()
if(NOT EXISTS ${OUT}/spec/chapters.md)
  message(FATAL_ERROR "missing chapters.md")
endif()
