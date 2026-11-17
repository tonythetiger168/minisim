# The OpenPiton-style many-core of bench/gen_manycore.py on msim: tiles with
# their own core, L1, L2 slice, directory and three NoC routers, running a
# shared-memory program. The testbench prints PASS only when the checksum the
# cores computed through the coherent memory matches the generator's own
# model, so this checks NoC routing, coherence (pointer and broadcast
# invalidations, fetch-and-increment) and the mcrv core together.
#   cmake -DMSIM=<msim> -DPY=<python> -DSRC=<source dir> -DOUT=<dir> -P msim_manycore.cmake
file(MAKE_DIRECTORY ${OUT})
foreach(cfg "1x1;--rounds;1" "3x2;--rounds;2" "2x2;--rounds;3;--dir-ptrs;1")
  list(GET cfg 0 mesh)
  list(SUBLIST cfg 1 -1 opts)
  execute_process(COMMAND ${PY} ${SRC}/bench/gen_manycore.py --mesh ${mesh} ${opts}
                  OUTPUT_FILE ${OUT}/mc_${mesh}.sv RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "gen_manycore.py --mesh ${mesh} failed")
  endif()
  foreach(th 1 4)
    execute_process(COMMAND ${MSIM} ${OUT}/mc_${mesh}.sv -top mc_tb -threads ${th}
                    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    string(REGEX MATCH "mc: [^\n]*" line "${out}")
    if(NOT rc EQUAL 0 OR NOT line MATCHES " PASS$")
      message(FATAL_ERROR "mesh ${mesh} -threads ${th}: rc=${rc} ${line}\n${err}")
    endif()
    message(STATUS "${line} (-threads ${th})")
  endforeach()
endforeach()
