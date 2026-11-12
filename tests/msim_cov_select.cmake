# msim -cm / -cm_hier: which code coverage is written, in three vendors'
# notations (src/cov_select.hpp), on tests/cov/fsm.sv (tb.u ctrl, tb.v one,
# tb.w ifs).
#   cmake -DPY=<python3> -DMSIM=<msim> -DSRC=<source dir> -DOUT=<dir> -P msim_cov_select.cmake
file(REMOVE_RECURSE ${OUT})
file(MAKE_DIRECTORY ${OUT})
# The counts of a database: lines branches toggles conditions fsms, and its FSM and toggle names.
file(WRITE ${OUT}/counts.py [=[
import json, sys
d = json.load(open(sys.argv[1]))
print(len(d["lines"]), len(d["branches"]), len(d["toggles"]), len(d["conditions"]), len(d["fsms"]), end="|")
print(" ".join(sorted(f["inst"] for f in d["fsms"])), end="|")
print(" ".join(sorted({t[0].rsplit(".", 1)[0] for t in d["toggles"]})), end="")
]=])
function(cov name want_fsms want_scopes)
  execute_process(COMMAND ${MSIM} ${SRC}/tests/cov/fsm.sv ${ARGN} -cov ${name}.json WORKING_DIRECTORY ${OUT}
                  RESULT_VARIABLE rc OUTPUT_QUIET ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "msim ${ARGN} exited ${rc}: ${err}")
  endif()
  execute_process(COMMAND ${PY} counts.py ${name}.json WORKING_DIRECTORY ${OUT} OUTPUT_VARIABLE o)
  string(REGEX MATCH "^([^|]*)\\|([^|]*)\\|(.*)$" m "${o}")
  set(n "${CMAKE_MATCH_1}")
  set(f "${CMAKE_MATCH_2}")
  set(s "${CMAKE_MATCH_3}")
  if(NOT f STREQUAL "${want_fsms}" OR NOT s STREQUAL "${want_scopes}")
    message(FATAL_ERROR "${name} (${ARGN}): FSMs '${f}' toggle scopes '${s}', expected '${want_fsms}' '${want_scopes}'")
  endif()
  set(counts "${n}" PARENT_SCOPE)
endfunction()

cov(all "tb.u tb.v tb.w" "tb tb.u tb.v tb.w")
set(all_counts "${counts}")
# The same metrics in each notation give the same database.
cov(vcs "tb.u tb.v tb.w" "" -cm fsm+cond)
if(NOT counts MATCHES "^0 0 0 [1-9][0-9]* 3$")
  message(FATAL_ERROR "-cm fsm+cond: ${counts}")
endif()
cov(xcelium "tb.u tb.v tb.w" "" -cm f:e:u)
cov(questa "tb.u tb.v tb.w" "" -cm=fce)
foreach(x xcelium questa)
  file(READ ${OUT}/vcs.json a)
  file(READ ${OUT}/${x}.json b)
  if(NOT a STREQUAL b)
    message(FATAL_ERROR "-cm in ${x} notation writes another database than VCS fsm+cond")
  endif()
endforeach()
cov(all2 "tb.u tb.v tb.w" "tb tb.u tb.v tb.w" -cm all)
if(NOT counts STREQUAL all_counts)
  message(FATAL_ERROR "-cm all: ${counts}, -cov alone: ${all_counts}")
endif()

# VCS -cm_hier.
file(WRITE ${OUT}/h1 "// leave out one instance\n-tree tb.v\n")
cov(h1 "tb.u tb.w" "tb tb.u tb.w" -cm_hier h1)
file(WRITE ${OUT}/h2 "+tree tb.u\n+module ifs   # by module\n")
cov(h2 "tb.u tb.w" "tb.u tb.w" -cm_hier h2)
file(WRITE ${OUT}/h3 "+tree tb 1\n")
cov(h3 "" "tb" -cm_hier h3)
# Xcelium covfile: per metric.
file(WRITE ${OUT}/cf "select_coverage -betf -instance tb.u\nselect_coverage -toggle -module one\nset_toggle_scoring -sv_enum\n")
cov(cf "tb.u" "tb.u tb.v" -cm b:e:t:f -cm_hier cf)
file(WRITE ${OUT}/cf2 "deselect_coverage -all -instance tb.w...\nset_expr_coverable_operators -all\n")
cov(cf2 "tb.u tb.v" "tb tb.u tb.v" -cm_hier cf2)

# The selection is the same with -compile.
execute_process(COMMAND ${MSIM} ${SRC}/tests/cov/fsm.sv -cm b:e:t:f -cm_hier cf -compile -o simv.d -cov cfc.json
                WORKING_DIRECTORY ${OUT} RESULT_VARIABLE rc OUTPUT_QUIET ERROR_VARIABLE err)
file(READ ${OUT}/cf.json a)
file(READ ${OUT}/cfc.json b)
string(REGEX REPLACE "\"tool\": \"[^\"]*\"" "" a "${a}")
string(REGEX REPLACE "\"tool\": \"[^\"]*\"" "" b "${b}")
if(NOT rc EQUAL 0 OR NOT a STREQUAL b)
  message(FATAL_ERROR "-compile writes another selection (${rc}): ${err}")
endif()

# Errors.
foreach(bad "-cm;line+bogus" "-cm;q" "-cm_hier;missing.cfg")
  execute_process(COMMAND ${MSIM} ${SRC}/tests/cov/fsm.sv ${bad} WORKING_DIRECTORY ${OUT} RESULT_VARIABLE rc ERROR_VARIABLE err OUTPUT_QUIET)
  if(NOT rc EQUAL 2)
    message(FATAL_ERROR "msim ${bad} should be a usage error: ${rc} ${err}")
  endif()
endforeach()
file(WRITE ${OUT}/bad "select_coverage -block\n")
execute_process(COMMAND ${MSIM} ${SRC}/tests/cov/fsm.sv -cm_hier bad WORKING_DIRECTORY ${OUT} RESULT_VARIABLE rc ERROR_VARIABLE err OUTPUT_QUIET)
if(NOT rc EQUAL 2 OR NOT err MATCHES "bad:1: select_coverage needs -instance or -module")
  message(FATAL_ERROR "a covfile line without a scope: ${rc} ${err}")
endif()
message(STATUS "msim -cm / -cm_hier: ok")
