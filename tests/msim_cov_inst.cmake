# Per-instance coverage: tests/cov/inst.sv has two instances of leaf, one of
# them (tb.u1.u) with b tied low. Checks the instance table, the per-instance
# condition vectors, that merging keeps them, that -compile and any -threads
# give the same database, and the inst exclusion.
#   cmake -DPY=<python3> -DMSIM=<msim> -DSRC=<source dir> -DOUT=<dir> -P msim_cov_inst.cmake
file(REMOVE_RECURSE ${OUT})
file(MAKE_DIRECTORY ${OUT})
set(SV ${SRC}/tests/cov/inst.sv)
set(MCOV ${PY} ${SRC}/tools/mcov.py)
function(expect what out)
  foreach(want ${ARGN})
    if(NOT out MATCHES "${want}")
      message(FATAL_ERROR "${what}: no match for '${want}' in\n${out}")
    endif()
  endforeach()
endfunction()
execute_process(COMMAND ${MSIM} ${SV} -cov a.json WORKING_DIRECTORY ${OUT} RESULT_VARIABLE rc OUTPUT_QUIET ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "msim -cov exited ${rc}: ${err}")
endif()
execute_process(COMMAND ${MCOV} report a.json -instances -detail WORKING_DIRECTORY ${OUT} OUTPUT_VARIABLE out)
set(table
  "tb +tb +100\\.0% +8/8 +- +- +[0-9.]+% +[0-9]+/[0-9]+ +- +-"
  "tb\\.u0 +leaf +100\\.0% +6/6 +100\\.0% +2/2 +100\\.0% +8/8 +100\\.0% +9/9 "
  "tb\\.u1 +mid +- +- +- +[0-9.]+% +[0-9]+/5 "
  "tb\\.u1\\.u +leaf +83\\.3% +5/6 +50\\.0% +1/2 +37\\.5% +3/8 +33\\.3% +3/9 ")
expect("one run" "${out}" ${table}
  "Total +100\\.0% +14/14 +75\\.0% +3/4"
  "Condition: 100\\.0% \\(8/8 vectors of 3 condition"
  "inst\\.sv:9:9  a && b\n[^\n]*\n[^\n]*\n[^\n]*\n     1   1   1\n     0   1   1\n     1   0   3\n    tb\\.u0: 1 1 1  \\(3/3\\)\n    tb\\.u1\\.u: 0 0 2  \\(1/3\\)"
  "p && q\n[^\n]*\n[^\n]*\n[^\n]*\n[^\n]*\n[^\n]*\n[^\n]*\n    tb\\.u0: 1 1 1  \\(3/3\\)\n    tb\\.u1\\.u: 0 0 2  \\(1/3\\)")
execute_process(COMMAND ${MCOV} report a.json -instances -uncovered WORKING_DIRECTORY ${OUT} OUTPUT_VARIABLE out)
if(out MATCHES "tb\\.u0: ")
  message(FATAL_ERROR "-uncovered -instances lists a fully covered instance:\n${out}")
endif()

# Merging two runs keeps the instances: same table, doubled hits.
execute_process(COMMAND ${MCOV} merge a.json a.json -o m.json WORKING_DIRECTORY ${OUT} RESULT_VARIABLE rc OUTPUT_QUIET)
execute_process(COMMAND ${MCOV} merge m.json -o m2.json WORKING_DIRECTORY ${OUT} RESULT_VARIABLE rc OUTPUT_QUIET)
execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files ${OUT}/m.json ${OUT}/m2.json RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "merging a merged database changed it")
endif()
execute_process(COMMAND ${MCOV} report m.json -instances -detail WORKING_DIRECTORY ${OUT} OUTPUT_VARIABLE out)
expect("merged" "${out}" ${table} "    tb\\.u1\\.u: 0 0 4  \\(1/3\\)")
execute_process(COMMAND ${MCOV} report a.json -html html WORKING_DIRECTORY ${OUT} OUTPUT_QUIET)
file(READ ${OUT}/html/index.html page)
expect("html" "${page}" "<h2>Instances</h2>" "<td>tb\\.u1\\.u</td><td>leaf</td><td class='part'>83\\.3% \\(5/6\\)</td>")

# The database does not depend on the thread count, nor on -compile.
execute_process(COMMAND ${MSIM} ${SV} -cov t1.json -threads 1 WORKING_DIRECTORY ${OUT} OUTPUT_QUIET)
execute_process(COMMAND ${MSIM} ${SV} -cov t3.json -threads 3 WORKING_DIRECTORY ${OUT} OUTPUT_QUIET)
foreach(other t1 t3)
  execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files ${OUT}/a.json ${OUT}/${other}.json RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "the database differs with ${other}")
  endif()
endforeach()
execute_process(COMMAND ${MSIM} ${SV} -compile -cov c.json WORKING_DIRECTORY ${OUT} RESULT_VARIABLE rc OUTPUT_QUIET ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "msim -compile -cov exited ${rc}: ${err}")
endif()
execute_process(COMMAND ${MCOV} report c.json -instances -detail WORKING_DIRECTORY ${OUT} OUTPUT_VARIABLE out)
expect("compiled" "${out}" ${table} "    tb\\.u1\\.u: 0 0 2  \\(1/3\\)")

# Excluding the tied-off instance: what only it had leaves, the rest stays.
file(WRITE ${OUT}/i.el "inst tb.u1 -- b is tied low here\n")
execute_process(COMMAND ${MCOV} report a.json -instances -detail -elfile i.el -el-review WORKING_DIRECTORY ${OUT} OUTPUT_VARIABLE out)
expect("inst exclusion" "${out}"
  "Total +100\\.0% +14/14 +100\\.0% +2/2"
  "Condition: 100\\.0% \\(8/8 vectors"
  "     1   0   1\n    tb\\.u0: 1 1 1"
  "i\\.el:1: inst tb\\.u1: [0-9]+ item\\(s\\)")
if(out MATCHES "tb\\.u1")
  string(REGEX MATCH "\ntb\\.u1[^\n]*" row "${out}")
  if(row)
    message(FATAL_ERROR "inst exclusion: the instance is still in the table:\n${out}")
  endif()
endif()
message(STATUS "per-instance coverage: ok")
