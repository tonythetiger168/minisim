# VCS partition compile spellings on msim (src/msim.cpp, docs/COMPAT.md):
# -partcomp / -fastpartcomp=j<n> run compiled mode, -partcomp_dir=<dir> and
# -Mdir=<dir> name its build directory, and a second run with nothing
# changed reuses the simv there. The output must match the interpreter's.
# The same options pass through the xrun front end.
#   cmake -DMSIM=<msim> -DSRC=<source dir> -DOUT=<dir> -P msim_partcomp.cmake
file(REMOVE_RECURSE ${OUT})
file(MAKE_DIRECTORY ${OUT})
set(DUT ${SRC}/examples/div.sv)

function(run)
  execute_process(COMMAND ${ARGN} WORKING_DIRECTORY ${OUT}
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${ARGN} exited ${rc}: ${err}\n${out}")
  endif()
  set(out "${out}" PARENT_SCOPE)
  set(err "${err}" PARENT_SCOPE)
endfunction()

run(${MSIM} ${DUT})
set(ref "${out}")

run(${MSIM} -partcomp=autopart_low -fastpartcomp=j2 -partcomp_dir=${OUT}/pc -O1 ${DUT})
if(NOT out STREQUAL ref)
  message(FATAL_ERROR "-partcomp printed\n${out}\nnot\n${ref}")
endif()
if(NOT err MATCHES "msim chooses its own parts")
  message(FATAL_ERROR "no partition note: ${err}")
endif()
if(NOT EXISTS ${OUT}/pc/simv)
  message(FATAL_ERROR "-partcomp_dir: no simv in ${OUT}/pc")
endif()
file(TIMESTAMP ${OUT}/pc/simv t1 "%s")

# Nothing changed: the simv is reused, not rebuilt.
run(${MSIM} -fastpartcomp -Mdir=${OUT}/pc -O1 ${DUT})
file(TIMESTAMP ${OUT}/pc/simv t2 "%s")
if(NOT out STREQUAL ref OR NOT t1 STREQUAL t2)
  message(FATAL_ERROR "second run: simv ${t1} -> ${t2}\n${out}")
endif()

# Through the xrun front end, unchanged.
run(${MSIM} -xrun -partcomp -partcomp_dir=${OUT}/pc -O1 ${DUT} -nolog)
file(TIMESTAMP ${OUT}/pc/simv t3 "%s")
if(NOT out STREQUAL ref OR NOT t1 STREQUAL t3)
  message(FATAL_ERROR "xrun -partcomp: simv ${t1} -> ${t3}\n${out}")
endif()
if(EXISTS ${OUT}/msim.d)
  message(FATAL_ERROR "-partcomp_dir was not used: ${OUT}/msim.d exists")
endif()
