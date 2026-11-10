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

# Bad plans are refused with the file and line.
file(WRITE ${OUT}/bad1.vplan "plan p\n  feature a\n    tset x\n")
file(WRITE ${OUT}/bad2.vplan "plan p\n  feature a\n  feature a\n")
file(WRITE ${OUT}/bad3.vplan "plan p\n  feature a\n    test x goal=120\n")
file(WRITE ${OUT}/bad4.vplan "plan p\n  include bad4.vplan\n")
foreach(n 1 2 3 4)
  execute_process(COMMAND ${MPLAN} check ${OUT}/bad${n}.vplan RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
  expect("bad plan ${n}" ${rc} 2 "${out}" "bad${n}\\.vplan(:[0-9]+)?: ")
endforeach()
