# mregress -farm slurm against a real one-node Slurm (CI starts it with
# tests/regress/slurm_up.sh); skips when no Slurm node is up.
#   cmake -DPY=<python3> -DMSIM=<msim> -DSRC=<source dir> -DOUT=<dir> -P mregress_slurm.cmake
execute_process(COMMAND ${PY} ${SRC}/tests/regress/slurm_test.py ${SRC}/tools/mregress.py ${MSIM}
                        ${SRC}/tests/regress/smoke.list ${OUT}
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
message(STATUS "${out}")
if(NOT rc EQUAL 0 OR NOT out MATCHES "slurm_test: (ok|skipped)")
  message(FATAL_ERROR "mregress -farm slurm: rc=${rc}\n${out}${err}")
endif()
