# mplan: a plan scored against a real regression (tests/plan), the history
# and trend, failure triage with rules, the gates, and bad plans.
#   cmake -DPY=<python3> -DMSIM=<msim> -DSRC=<source dir> -DOUT=<dir> -P mplan.cmake
file(REMOVE_RECURSE ${OUT})
file(MAKE_DIRECTORY ${OUT})
set(MPLAN ${PY} ${SRC}/tools/mplan.py)
set(MREGRESS ${PY} ${SRC}/tools/mregress.py)
set(PLAN ${SRC}/tests/plan/counter.vplan)
set(LIST ${SRC}/tests/plan/plan.list)

function(expect what rc want out)
  if(NOT rc EQUAL want)
    message(FATAL_ERROR "${what}: exit ${rc}, expected ${want}\n${out}")
  endif()
  foreach(rx ${ARGN})
    if(NOT out MATCHES "${rx}")
      message(FATAL_ERROR "${what}: output does not match '${rx}'\n${out}")
    endif()
  endforeach()
endfunction()

execute_process(COMMAND ${MPLAN} check ${PLAN} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
expect("check" ${rc} 0 "${out}" "5 feature\\(s\\), 10 metric\\(s\\)" "nothing mapped yet: lowpower")

# Snapshot c0: only two of the tests ran. Coverage holes show in the plan.
execute_process(COMMAND ${MREGRESS} ${LIST} -exe ${MSIM} -cov -seed 1 -q -o ${OUT}/r0 -filter "count_(up|down)"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
expect("regression 0" ${rc} 0 "${out}" "Functional: 91\\.7%" "Assertions: 66\\.7% \\(2/3")
execute_process(COMMAND ${MPLAN} report ${PLAN} -results ${OUT}/r0 -o ${OUT}/plan -label c0 -time 2026-11-01T00:00:00Z
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
expect("report 0" ${rc} 0 "${out}"
       "cover tb\\.cg\\.x_q_dir +75\\.0% +met +3/4  goal 75%"
       "assert tb\\.dut\\.c_wrap +0\\.0% +below goal +0/1  \\(1 never passed or hit\\)"
       "test sb_\\* +0\\.0% +below goal +0/1  \\(no test matches\\)"
       "lowpower +0\\.0% +empty"
       "mplan: counter: 45\\.3% \\(partial\\), 0/5 features met")

# Snapshot c1: the whole list, through mregress -plan. One test fails.
execute_process(COMMAND ${MREGRESS} ${LIST} -exe ${MSIM} -cov -seed 1 -q -o ${OUT}/r1 -plan ${PLAN}
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
expect("regression 1" ${rc} 1 "${out}" "mplan: counter: 59\\.5% \\(failing\\), 3/5 features met")
if(NOT EXISTS ${OUT}/r1/plan/plan.html OR NOT EXISTS ${OUT}/r1/plan/history.jsonl)
  message(FATAL_ERROR "mregress -plan wrote no plan report")
endif()
execute_process(COMMAND ${MPLAN} report ${PLAN} -results ${OUT}/r1 -o ${OUT}/plan -label c1 -time 2026-11-02T00:00:00Z
                        -fail-on-new -gate -min-score 60 -md ${OUT}/plan.md
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
expect("report 1" ${rc} 1 "${out}"
       "count +100\\.0% +met"
       "wrap +100\\.0% +met +alice"
       "line counter\\.sv +90\\.9% +met +20/22  goal 90%"
       "scoreboard +0\\.0% +failing +bob"
       "3/5 features met, \\+14\\.2 since c0"
       "failure buckets: 1 \\(1 new; compared to c0\\)"
       "\\[new\\]  Error: scoreboard mismatch at <hex>, expected <n>"
       "features: scoreboard; owner: bob"
       "newly failing tests: sb_check"
       "gate: the plan is failing, not met"
       "gate: plan score 59\\.5% is below 60%"
       "gate: 1 new failure bucket")
file(READ ${OUT}/plan.md md)
if(NOT md MATCHES "Wrap around \\| 100\\.0% \\| met \\| alice")
  message(FATAL_ERROR "Markdown table wrong:\n${md}")
endif()

# The same label again replaces the snapshot; a rule marks the bucket known, so -fail-on-new passes.
execute_process(COMMAND ${MPLAN} report ${PLAN} -results ${OUT}/r1 -o ${OUT}/plan -label c1 -time 2026-11-02T00:00:00Z
                        -rules ${SRC}/tests/plan/triage.rules -fail-on-new -threads 1
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
expect("report 1 with rules" ${rc} 0 "${out}" "\\[new, known\\]" "owner: carol; bug: BUG-7; note: reference model is off by one")
file(READ ${OUT}/plan/plan.json one)
execute_process(COMMAND ${MPLAN} report ${PLAN} -results ${OUT}/r1 -o ${OUT}/plan -label c1 -time 2026-11-02T00:00:00Z
                        -rules ${SRC}/tests/plan/triage.rules -threads 4 -q
                RESULT_VARIABLE rc OUTPUT_QUIET)
file(READ ${OUT}/plan/plan.json four)
if(NOT one STREQUAL four)
  message(FATAL_ERROR "plan.json differs between -threads 1 and -threads 4")
endif()

# Snapshot c2: the same failure is now recurring. Snapshot c3: it is gone.
execute_process(COMMAND ${MPLAN} report ${PLAN} -results ${OUT}/r1 -o ${OUT}/plan -label c2 -time 2026-11-03T00:00:00Z
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
expect("report 2" ${rc} 0 "${out}" "\\+0\\.0 since c1" "\\[recurring, since c1\\]")
execute_process(COMMAND ${MREGRESS} ${LIST} -exe ${MSIM} -cov -seed 1 -q -o ${OUT}/r3 -filter count
                RESULT_VARIABLE rc OUTPUT_QUIET ERROR_QUIET)
execute_process(COMMAND ${MPLAN} report ${PLAN} -results ${OUT}/r3 -o ${OUT}/plan -label c3 -time 2026-11-04T00:00:00Z
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
expect("report 3" ${rc} 0 "${out}" "regressed: reset 97\\.7% -> 92\\.2%" "0 new, 1 fixed; compared to c2"
       "fixed: Error: scoreboard mismatch")

execute_process(COMMAND ${MPLAN} trend ${OUT}/plan/history.jsonl RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
expect("trend" ${rc} 0 "${out}" "c0 +45\\.3% +2/2 +0 " "c1 +59\\.5% +\\+14\\.2 +3/4 +1 " "c3 +58\\.[45]% +-1\\.[01] +3/3 +0 ")
execute_process(COMMAND ${MPLAN} trend ${OUT}/plan/history.jsonl -feature count.wrap -n 2
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
expect("trend of a feature" ${rc} 0 "${out}" "c3 +100\\.0% +\\+0\\.0")

execute_process(COMMAND ${MPLAN} triage -results ${OUT}/r1 -plan ${PLAN} -history ${OUT}/plan/history.jsonl -fail-on-new
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
expect("triage" ${rc} 1 "${out}" "\\[new\\]  Error: scoreboard mismatch" "features: scoreboard; owner: bob")

execute_process(COMMAND ${MPLAN} init -results ${OUT}/r1 -name counter -o ${OUT}/draft.vplan
                RESULT_VARIABLE rc OUTPUT_QUIET)
execute_process(COMMAND ${MPLAN} report ${OUT}/draft.vplan -results ${OUT}/r1 -o ${OUT}/draft -no-history
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
expect("init" ${rc} 0 "${out}" "test count\\* +100\\.0% +met +3/3" "cover tb\\.cg +100\\.0%" "assert tb\\.dut +100\\.0% +met +3/3")
if(EXISTS ${OUT}/draft/history.jsonl)
  message(FATAL_ERROR "-no-history wrote a history")
endif()

# The HTML report holds the tree, the trend chart and the triage table.
file(READ ${OUT}/plan/plan.html page)
foreach(rx "Counter verification plan" "<svg id='trend'" "Wrap around" "Failure triage" "Not in the plan|No failing runs")
  if(NOT page MATCHES "${rx}")
    message(FATAL_ERROR "plan.html lacks '${rx}'")
  endif()
endforeach()

# FSM and condition metrics (tests/cov/fsm.sv): one run, then both.
file(WRITE ${OUT}/fsm.list "a: ${SRC}/tests/cov/fsm.sv -top tb\nb: ${SRC}/tests/cov/fsm.sv -top tb +define+MORE\n")
file(WRITE ${OUT}/fsm.vplan "plan fsm\n  feature ctrl\n    fsm tb.u.state goal=80\n    cond fsm.sv goal=60\n  feature all\n    fsm tb\n")
execute_process(COMMAND ${MREGRESS} ${OUT}/fsm.list -exe ${MSIM} -cov -seed 1 -q -o ${OUT}/f0 -filter "^a$"
                RESULT_VARIABLE rc OUTPUT_QUIET ERROR_QUIET)
execute_process(COMMAND ${MPLAN} report ${OUT}/fsm.vplan -results ${OUT}/f0 -o ${OUT}/fplan -label f0
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
expect("fsm report 0" ${rc} 0 "${out}" "fsm tb\\.u\\.state +60\\.0% +below goal +6/10  goal 80%"
       "cond fsm\\.sv +50\\.0% +below goal +9/18  goal 60%" "fsm tb +70\\.8% +below goal +17/24" "cond 50\\.0%, fsm 70\\.8%")
execute_process(COMMAND ${MREGRESS} ${OUT}/fsm.list -exe ${MSIM} -cov -seed 1 -q -o ${OUT}/f1
                RESULT_VARIABLE rc OUTPUT_QUIET ERROR_QUIET)
execute_process(COMMAND ${MPLAN} report ${OUT}/fsm.vplan -results ${OUT}/f1 -o ${OUT}/fplan -label f1
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
expect("fsm report 1" ${rc} 0 "${out}" "fsm tb\\.u\\.state +90\\.0% +met +9/10  goal 80%" "fsm tb +87\\.5% +below goal +21/24"
       "\\+17\\.2 since f0")
execute_process(COMMAND ${MPLAN} init -results ${OUT}/f1 RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
expect("fsm init" ${rc} 0 "${out}" "fsm tb\\.u\\.state" "cond fsm\\.sv")

# Bad plans are refused with the file and line.
file(WRITE ${OUT}/bad1.vplan "plan p\n  feature a\n    tset x\n")
file(WRITE ${OUT}/bad2.vplan "plan p\n  feature a\n  feature a\n")
file(WRITE ${OUT}/bad3.vplan "plan p\n  feature a\n    test x goal=120\n")
file(WRITE ${OUT}/bad4.vplan "plan p\n  include bad4.vplan\n")
foreach(n 1 2 3 4)
  execute_process(COMMAND ${MPLAN} check ${OUT}/bad${n}.vplan RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
  expect("bad plan ${n}" ${rc} 2 "${out}" "bad${n}\\.vplan(:[0-9]+)?: ")
endforeach()

# Requirements: a CSV list named by the plan, the matrix, the gate, the trend column.
set(TRACED ${SRC}/tests/plan/traced.vplan)
execute_process(COMMAND ${MPLAN} check ${TRACED} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
expect("reqs check" ${rc} 0 "${out}" "6 requirement\\(s\\), 5 named by a feature" "no feature verifies: CNT-5  Parity output"
       "warning: feature reset names requirement 'CNT-9'")
execute_process(COMMAND ${MPLAN} report ${TRACED} -results ${OUT}/r1 -o ${OUT}/tplan -label t1 -trace ${OUT}/trace.csv -fail-on-uncovered
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
expect("reqs report" ${rc} 1 "${out}" "requirements: 3/6 verified, 1 not started, 1 failing, 1 uncovered"
       "failing +CNT-3  Scoreboard agrees  \\(scoreboard 0\\.0%\\)" "uncovered +CNT-5  Parity output"
       "gate: 1 uncovered requirement\\(s\\), 1 unknown id\\(s\\)")
file(READ ${OUT}/trace.csv csv)
foreach(rx "CNT-1,Counting,,,verified,100\\.0,,," "CNT-1\\.2,Wrap around,CNT-1,high,verified,100\\.0,count\\.wrap,,count_wrap"
           "CNT-3,Scoreboard agrees,,medium,failing,0\\.0,scoreboard,,sb_\\*" "CNT-5,Parity output,,low,uncovered,0\\.0,,,")
  if(NOT csv MATCHES "${rx}")
    message(FATAL_ERROR "trace.csv lacks '${rx}'\n${csv}")
  endif()
endforeach()
file(READ ${OUT}/tplan/plan.html page)
foreach(rx "<h2>Requirements</h2>" "requirements verified \\(1 uncovered\\)" "verifies CNT-1\\.2")
  if(NOT page MATCHES "${rx}")
    message(FATAL_ERROR "plan.html lacks '${rx}'")
  endif()
endforeach()
execute_process(COMMAND ${MPLAN} trend ${OUT}/tplan/history.jsonl RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
expect("reqs trend" ${rc} 0 "${out}" "Buckets +Reqs" " 3/6 ")

# The same requirements from ReqIF (-reqs), with the hierarchy and the XHTML text; a draft plan from them.
file(WRITE ${OUT}/rq.vplan "plan rq\n  feature count\n    req CNT-1.1 CNT-1.2\n    test count_*\n  feature sb\n    req CNT-2\n    test sb_*\n")
execute_process(COMMAND ${MPLAN} report ${OUT}/rq.vplan -reqs ${SRC}/tests/plan/reqs.reqif -results ${OUT}/r1 -o ${OUT}/rqplan -no-history
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
expect("reqif report" ${rc} 0 "${out}" "requirements: 2/3 verified, 1 failing" "failing +CNT-2  Reset  \\(sb 0\\.0%\\)")
file(READ ${OUT}/rqplan/plan.json js)
if(NOT js MATCHES "\"id\": \"CNT-1\\.1\", \"parent\": \"CNT-1\"" OR NOT js MATCHES "q follows dir, one step per clock")
  message(FATAL_ERROR "plan.json lacks the ReqIF hierarchy or text")
endif()
execute_process(COMMAND ${MPLAN} init -reqs ${SRC}/tests/plan/reqs.reqif -name cnt RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
expect("reqif init" ${rc} 0 "${out}" "feature CNT-1 \"Counting\"\n    feature CNT-1_1 \"Count up and down\"\n      req CNT-1\\.1" "feature CNT-2 \"Reset\"\n    req CNT-2")
file(WRITE ${OUT}/bad.csv "name,text\nx,y\n")
execute_process(COMMAND ${MPLAN} check ${OUT}/rq.vplan -reqs ${OUT}/bad.csv RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
expect("bad reqs" ${rc} 2 "${out}" "bad\\.csv: no 'id' column")

# Ranking: what each passing test adds (per-run coverage databases), the redundant ones, the tests behind a feature.
execute_process(COMMAND ${MPLAN} rank ${PLAN} -results ${OUT}/r1 -keep ${OUT}/keep.list -json ${OUT}/rank.json
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
expect("rank" ${rc} 0 "${out}" "1  count_down +54 +39\\.4%" "2  count_wrap +7 +44\\.5%" "3  count_up +2 +46\\.0%"
       "3 of 3 tests reach 46\\.0% \\(63/137 points\\)" "not ranked \\(not passing\\): sb_check"
       "count\\.wrap: count_wrap \\(1\\)" "reset: count_down \\(31\\), count_wrap \\(29\\), count_up \\(25\\)")
file(READ ${OUT}/keep.list keep)
if(NOT keep STREQUAL "count_down\ncount_wrap\ncount_up\n")
  message(FATAL_ERROR "keep list wrong:\n${keep}")
endif()
execute_process(COMMAND ${MPLAN} rank -results ${OUT}/r1 -all -threads 1 RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
expect("rank -all" ${rc} 0 "${out}" "4 of 4 tests reach 48\\.9% \\(67/137 points\\)")
execute_process(COMMAND ${MPLAN} rank -results ${OUT}/f1 RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
expect("rank redundant" ${rc} 0 "${out}" "1  b +170 +87\\.6%" "redundant \\(add nothing\\): a")
execute_process(COMMAND ${MPLAN} report ${PLAN} -results ${OUT}/r1 -o ${OUT}/kplan -no-history -rank -q
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
file(READ ${OUT}/kplan/plan.html page)
file(READ ${OUT}/kplan/plan.json js)
if(NOT page MATCHES "<h2>Tests by what they add</h2>" OR NOT js MATCHES "\"ranking\": {\"by\": \"points\"")
  message(FATAL_ERROR "report -rank: no ranking in plan.html / plan.json")
endif()
execute_process(COMMAND ${MPLAN} rank -results ${OUT}/r3x RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
if(rc EQUAL 0)
  message(FATAL_ERROR "rank on a missing directory passed")
endif()
