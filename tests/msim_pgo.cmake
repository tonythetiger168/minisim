# Profile-guided builds of compiled mode (src/msim.cpp pgoFlags, docs/COMPILE.md):
# -pgo-gen builds a simv that writes gcc profiles into <dir>/pgo when it runs,
# -pgo-use rebuilds with them. Both print what the interpreter prints, for a
# one-file build and a build split into parts (-cfiles 2).
#   cmake -DMSIM=<msim> -DSRC=<source dir> -DOUT=<dir> -P msim_pgo.cmake
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

foreach(split "" "-cfiles;2")
  set(d ${OUT}/b${split})
  string(REPLACE ";" "" d "${d}")
  run(${MSIM} -compile -pgo-gen -o ${d} ${split} ${DUT})
  if(NOT out STREQUAL ref)
    message(FATAL_ERROR "-pgo-gen ${split} printed\n${out}\nnot\n${ref}")
  endif()
  file(GLOB_RECURSE prof ${d}/pgo/*.gcda)
  if(NOT prof)
    message(FATAL_ERROR "-pgo-gen ${split}: no profile under ${d}/pgo")
  endif()
  run(${MSIM} -compile -pgo-use -o ${d} ${split} ${DUT})
  if(NOT out STREQUAL ref)
    message(FATAL_ERROR "-pgo-use ${split} printed\n${out}\nnot\n${ref}")
  endif()
  if(err MATCHES "no profile")
    message(FATAL_ERROR "-pgo-use ${split} found no profile: ${err}")
  endif()
endforeach()

# -pgo-use without a profile still builds, and says why it is not used.
run(${MSIM} -compile -pgo-use -o ${OUT}/none ${DUT})
if(NOT out STREQUAL ref OR NOT err MATCHES "no profile")
  message(FATAL_ERROR "-pgo-use without a profile: ${err}\n${out}")
endif()
