# mcov exclusion files and test ranking, on tests/cov/fsm.sv run twice (the
# second run with +define+MORE covers more).
#   - mcov exclude drafts one exclusion per item not covered; with the draft
#     applied everything is 100%, and -el-strict rejects it (no reasons yet).
#   - a reviewed file: each kind of exclusion changes the numbers as expected;
#     -el-review flags a stale exclusion and one that removes covered items.
#   - mcov rank orders the tests by what each adds, the same for any -j.
#   cmake -DPY=<python3> -DMSIM=<msim> -DSRC=<source dir> -DOUT=<dir> -P mcov_excl.cmake
file(REMOVE_RECURSE ${OUT})
file(MAKE_DIRECTORY ${OUT})
set(SV ${SRC}/tests/cov/fsm.sv)
set(MCOV ${PY} ${SRC}/tools/mcov.py)
execute_process(COMMAND ${MSIM} ${SV} -cov a.json WORKING_DIRECTORY ${OUT} RESULT_VARIABLE rc OUTPUT_QUIET ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "msim -cov exited ${rc}: ${err}")
endif()
execute_process(COMMAND ${MSIM} ${SV} +define+MORE -cov b.json WORKING_DIRECTORY ${OUT} RESULT_VARIABLE rc OUTPUT_QUIET ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "msim -cov +define+MORE exited ${rc}: ${err}")
endif()

function(expect what out)
  foreach(want ${ARGN})
    if(NOT out MATCHES "${want}")
      message(FATAL_ERROR "${what}: no match for '${want}' in\n${out}")
    endif()
  endforeach()
endfunction()

# The draft, and the draft applied.
execute_process(COMMAND ${MCOV} exclude a.json -o draft.el WORKING_DIRECTORY ${OUT} RESULT_VARIABLE rc OUTPUT_VARIABLE out)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "mcov exclude exited ${rc}: ${out}")
endif()
file(READ ${OUT}/draft.el draft)
expect("draft" "${draft}"
  "line +[^\n]*fsm\\.sv:19  -- TODO"
  "branch [^\n]*fsm\\.sv:14  -- TODO"
  "cond +[^\n]*fsm\\.sv:30 2  -- TODO   # a && b"
  "cond +[^\n]*fsm\\.sv:32  -- TODO   # a && b && c"
  "state  tb\\.u\\.state FAIL  -- TODO"
  "trans  tb\\.u\\.state RUN->IDLE  -- TODO"
  "toggle tb\\.")
if(draft MATCHES "trans  tb\\.u\\.state RUN->FAIL")
  message(FATAL_ERROR "draft: a transition into an excluded state is listed again\n${draft}")
endif()
execute_process(COMMAND ${MCOV} report a.json -elfile draft.el -el-strict WORKING_DIRECTORY ${OUT} RESULT_VARIABLE rc OUTPUT_VARIABLE out)
expect("draft applied" "${out}"
  "Total +100\\.0%[^\n]*100\\.0%"
  "Toggle: 100\\.0%"
  "Condition: 100\\.0%"
  "FSM: states 100\\.0% \\(9/9\\), transitions 100\\.0% \\(8/8\\)"
  "no reason given"
  "exclusion\\(s\\) need review")
if(NOT rc EQUAL 1)
  message(FATAL_ERROR "-el-strict with TODO reasons should exit 1, got ${rc}")
endif()
# What the draft leaves is nothing: a second draft on top of it is empty.
execute_process(COMMAND ${MCOV} exclude a.json -elfile draft.el -o again.el WORKING_DIRECTORY ${OUT} OUTPUT_VARIABLE out)
expect("second draft" "${out}" "mcov: 0 exclusion\\(s\\) to review")

# A reviewed file.
file(WRITE ${OUT}/ok.el "# reviewed
state  tb.u.state FAIL          -- error injection is not in this regression
trans  tb.*.st S?->S0           -- reset only
cond   fsm.sv:30 3              -- b is tied high in this configuration
cond   tests/cov/fsm.sv:32      -- c is tied low
line   fsm.sv:19                -- FAIL arm
")
execute_process(COMMAND ${MCOV} report a.json -detail WORKING_DIRECTORY ${OUT} OUTPUT_VARIABLE before)
expect("before" "${before}" "Condition: 50\\.0% \\(9/18 vectors of 6" "FSM: states 90\\.0% \\(9/10\\), transitions 57\\.1% \\(8/14\\)")
execute_process(COMMAND ${MCOV} report a.json -elfile ok.el -el-review -el-strict -detail WORKING_DIRECTORY ${OUT} RESULT_VARIABLE rc OUTPUT_VARIABLE out)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "reviewed exclusions should pass -el-strict, got ${rc}:\n${out}")
endif()
expect("reviewed" "${out}"
  # FAIL and its two transitions, S1->S0 and S2->S0: 9 states, 14 - 4 transitions
  "FSM: states 100\\.0% \\(9/9\\), transitions 80\\.0% \\(8/10\\)"
  # one vector of a && b, all four of a && b && c: 18 - 5
  "Condition: 69\\.2% \\(9/13 vectors of 5"
  "Excluded: 11 item\\(s\\) by 5 exclusion\\(s\\)"
  "ok\\.el:2: state tb\\.u\\.state FAIL: 3 item\\(s\\)  -- error injection"
  "ok\\.el:3: trans tb\\.\\*\\.st S\\?->S0: 2 item\\(s\\)"
  "     1   0   Excluded")
if(out MATCHES "    state FAIL" OR out MATCHES "\\*\\* ")
  message(FATAL_ERROR "reviewed: FAIL still reported, or an exclusion flagged:\n${out}")
endif()

# Stale, and covered.
file(WRITE ${OUT}/bad.el "state tb.u.state NOPE -- gone\nstate tb.u.state RUN -- wrong\ntoggle tb.nothing_* -- gone\n")
execute_process(COMMAND ${MCOV} report a.json -elfile bad.el -el-review -el-strict WORKING_DIRECTORY ${OUT} RESULT_VARIABLE rc OUTPUT_VARIABLE out)
expect("bad" "${out}"
  "bad\\.el:1: [^\n]*\\*\\* matches nothing"
  "bad\\.el:2: [^\n]*\\*\\* [0-9]+ of its [0-9]+ item\\(s\\) are covered by the tests"
  "bad\\.el:3: [^\n]*\\*\\* matches nothing"
  "3 exclusion\\(s\\) need review")
if(NOT rc EQUAL 1)
  message(FATAL_ERROR "-el-strict with stale exclusions should exit 1, got ${rc}")
endif()
file(WRITE ${OUT}/syntax.el "stat tb.u.state FAIL\n")
execute_process(COMMAND ${MCOV} report a.json -elfile syntax.el WORKING_DIRECTORY ${OUT} RESULT_VARIABLE rc OUTPUT_QUIET ERROR_VARIABLE err)
if(rc EQUAL 0 OR NOT err MATCHES "syntax\\.el:1: expected")
  message(FATAL_ERROR "a bad exclusion kind should be an error, got ${rc}: ${err}")
endif()

# A bit range of a signal.
execute_process(COMMAND ${MCOV} report a.json WORKING_DIRECTORY ${OUT} OUTPUT_VARIABLE out)
string(REGEX MATCH "Toggle: [^\n]*\\(([0-9]+)/([0-9]+) bits" m "${out}")
set(th ${CMAKE_MATCH_1})
set(ta ${CMAKE_MATCH_2})
file(WRITE ${OUT}/bits.el "toggle tb.u.state[1] -- test\n")
execute_process(COMMAND ${MCOV} report a.json -elfile bits.el WORKING_DIRECTORY ${OUT} OUTPUT_VARIABLE out)
math(EXPR th1 "${th} - 1")
math(EXPR ta1 "${ta} - 1")
expect("bit range" "${out}" "Toggle: [^\n]*\\(${th1}/${ta1} bits")

# Ranking: b covers all that a covers and more.
execute_process(COMMAND ${MCOV} rank a.json b.json a.json -o keep.txt -j 1 WORKING_DIRECTORY ${OUT} RESULT_VARIABLE rc OUTPUT_VARIABLE r1)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "mcov rank exited ${rc}: ${r1}")
endif()
expect("rank" "${r1}"
  "   1  b\\.json +([0-9]+) +[1-9][0-9]* +([0-9]+) "
  "   2  a\\.json +[0-9]+ +0 +0 "
  "3 test\\(s\\), [0-9]+ item\\(s\\); the first 1 reach the total, 2 add nothing")
file(READ ${OUT}/keep.txt keep)
if(NOT keep STREQUAL "b.json\n")
  message(FATAL_ERROR "rank -o: expected only b.json, got '${keep}'")
endif()
execute_process(COMMAND ${MCOV} rank a.json b.json a.json -j 1 WORKING_DIRECTORY ${OUT} OUTPUT_VARIABLE r1)
execute_process(COMMAND ${MCOV} rank a.json b.json a.json -j 3 WORKING_DIRECTORY ${OUT} OUTPUT_VARIABLE r3)
if(NOT r1 STREQUAL r3)
  message(FATAL_ERROR "rank depends on -j:\n${r1}\n---\n${r3}")
endif()
execute_process(COMMAND ${MCOV} rank a.json b.json -item tb.u.state WORKING_DIRECTORY ${OUT} RESULT_VARIABLE rc OUTPUT_VARIABLE out)
expect("rank -item" "${out}"
  "tb\\.u\\.state state 3: b\\.json\n"
  "tb\\.u\\.state state 1: a\\.json, b\\.json\n"
  "tb\\.u\\.state transition 1->0: no test\n")
message(STATUS "mcov exclusions and ranking: ok")
