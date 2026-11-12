# msim -restore <checkpoint> (src/annexd.inc): a $save checkpoint keeps the
# command line, the directory and a digest of the state; -restore runs the
# simulation again quietly up to the $save and carries on from there, with
# the plusargs added after the file. A plusarg that changes the run before
# the checkpoint makes a different state, and msim says so.
#   cmake -DMSIM=<msim> -DSRC=<source dir> -DOUT=<dir> -P msim_restore.cmake
file(REMOVE_RECURSE ${OUT})
file(MAKE_DIRECTORY ${OUT})
file(COPY ${SRC}/tests/restore/count.sv DESTINATION ${OUT})

function(run)
  execute_process(COMMAND ${ARGN} WORKING_DIRECTORY ${SRC}
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${ARGN} exited ${rc}: ${err}\n${out}")
  endif()
  set(out "${out}" PARENT_SCOPE)
  set(err "${err}" PARENT_SCOPE)
endfunction()
function(expect text re)
  if(NOT text MATCHES "${re}")
    message(FATAL_ERROR "no match for '${re}' in\n${text}")
  endif()
endfunction()

execute_process(COMMAND ${MSIM} count.sv WORKING_DIRECTORY ${OUT} RESULT_VARIABLE rc OUTPUT_VARIABLE out)
expect("${out}" "^before 5\nafter save 5 late=0\nend 10\n$")
file(READ ${OUT}/count.chk chk)
expect("${chk}" "^msim checkpoint\nsave 1\ntime 52\nstate [0-9a-f]+\ncwd [^\n]+\narg [^\n]*msim\narg count.sv\n$")

# From another directory: the run goes back to the checkpoint's directory.
run(${MSIM} -restore ${OUT}/count.chk +late)
expect("${out}" "^after save 5 late=1\nend 10\n$")
expect("${err}" "restored the checkpoint at time 52")

# +early changes the run before the checkpoint: a warning, then the run goes on.
run(${MSIM} -restore ${OUT}/count.chk +early)
expect("${err}" "differs from the one saved")
expect("${out}" "^after save 10 late=0\nend 20\n$")

# Not a checkpoint.
execute_process(COMMAND ${MSIM} -restore ${SRC}/tests/restore/count.sv RESULT_VARIABLE rc ERROR_VARIABLE err)
if(NOT rc EQUAL 2)
  message(FATAL_ERROR "-restore of a source file exited ${rc}")
endif()
expect("${err}" "is not a checkpoint")
