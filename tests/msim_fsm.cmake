# FSM and condition coverage (msim -cov, as VCS -cm fsm+cond): tests/cov/fsm.sv
# has three FSMs (two-process on an enum, one-process on localparams, and one
# written with if (state == ...)), a counter that must not be taken for one,
# and six conditions. Checks the extraction, the counts of one run and of two
# merged runs, the reset filter, the thresholds, that the database does not
# depend on the thread count, and that -compile gives the same database.
#   cmake -DPY=<python3> -DMSIM=<msim> -DSRC=<source dir> -DOUT=<dir> -P msim_fsm.cmake
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

execute_process(COMMAND ${MCOV} report a.json -detail WORKING_DIRECTORY ${OUT} RESULT_VARIABLE rc OUTPUT_VARIABLE out)
foreach(want
    "Condition: 50\\.0% \\(9/18 vectors of 6 condition\\(s\\)\\)"
    "FSM: states 90\\.0% \\(9/10\\), transitions 57\\.1% \\(8/14\\) of 3 FSM\\(s\\)"
    "FSM tb\\.u\\.state  \\(module ctrl, [^\n]*fsm\\.sv:8\\)  states 3/4, transitions 3/6"
    "state FAIL +Not covered"
    "IDLE -> RUN +1\n"
    "RUN -> IDLE +Not covered  \\(reset\\)"
    "RUN -> FAIL +Not covered"
    "FSM tb\\.v\\.st  \\(module one, [^\n]*\\)  states 3/3, transitions 2/4"
    "S1 -> S2 +1\n"
    "FSM tb\\.w\\.ph  \\(module ifs, [^\n]*\\)  states 3/3, transitions 3/4"
    "2'h1 -> 2'h0 +Not covered"
    "fsm\\.sv:30:15  a && b\n    -1- a\n    -2- b\n    -1- -2-  Hits\n     1   1   1\n     0   1   Not covered\n     1   0   Not covered"
    "\\(state == RUN\\) \\|\\| \\(state == WAIT\\)\n[^\n]*\n[^\n]*\n[^\n]*\n     0   0   2\n     1   0   1\n     0   1   1")
  if(NOT out MATCHES "${want}")
    message(FATAL_ERROR "one run: no match for '${want}' in\n${out}")
  endif()
endforeach()
if(out MATCHES "FSM [^\n]*cnt")
  message(FATAL_ERROR "the counter was taken for an FSM\n${out}")
endif()

# Two runs merged; the reset filter drops RUN -> IDLE (only the reset makes it).
execute_process(COMMAND ${MCOV} merge a.json b.json -o all.json WORKING_DIRECTORY ${OUT} RESULT_VARIABLE rc OUTPUT_QUIET)
execute_process(COMMAND ${MCOV} report all.json -uncovered -fsm-reset-filter -min-fsm-state 100 -min-cond 60 -html html
                WORKING_DIRECTORY ${OUT} RESULT_VARIABLE rc OUTPUT_VARIABLE out)
if(NOT rc EQUAL 1 OR NOT out MATCHES "FSM: states 100\\.0% \\(10/10\\), transitions 84\\.6% \\(11/13\\) of 3 FSM\\(s\\)"
   OR NOT out MATCHES "cond coverage 55\\.6% is below 60" OR out MATCHES "fsm-state coverage" OR out MATCHES "\\(reset\\)")
  message(FATAL_ERROR "merged (rc ${rc})\n${out}")
endif()
file(READ ${OUT}/html/index.html page)
if(NOT page MATCHES "tb\\.u\\.state" OR NOT page MATCHES "IDLE &rarr; RUN")
  message(FATAL_ERROR "the HTML report has no FSM section")
endif()
execute_process(COMMAND ${MCOV} report a.json b.json WORKING_DIRECTORY ${OUT} OUTPUT_VARIABLE two)
execute_process(COMMAND ${MCOV} report all.json WORKING_DIRECTORY ${OUT} OUTPUT_VARIABLE one)
if(NOT two STREQUAL one OR NOT one MATCHES "transitions 78\\.6% \\(11/14\\)")
  message(FATAL_ERROR "a merged database reports differently from its parts\n${two}---\n${one}")
endif()

# Parallel partitions and worker threads: the same database for any -threads.
foreach(n 1 2 all)
  execute_process(COMMAND ${MSIM} ${SV} +define+MORE -threads ${n} -cov t${n}.json WORKING_DIRECTORY ${OUT}
                  RESULT_VARIABLE rc OUTPUT_QUIET ERROR_VARIABLE err)
  file(READ ${OUT}/t${n}.json t)
  file(READ ${OUT}/b.json b)
  if(NOT rc EQUAL 0 OR NOT t STREQUAL b)
    message(FATAL_ERROR "-threads ${n}: the coverage database differs (rc ${rc}) ${err}")
  endif()
endforeach()

# mcov merges in parallel worker processes: the same result for any -j.
execute_process(COMMAND ${MCOV} merge a.json b.json a.json b.json a.json b.json a.json b.json a.json -j 4 -o par.json
                WORKING_DIRECTORY ${OUT} RESULT_VARIABLE rc OUTPUT_QUIET)
execute_process(COMMAND ${MCOV} merge a.json b.json a.json b.json a.json b.json a.json b.json a.json -j 1 -o ser.json
                WORKING_DIRECTORY ${OUT} RESULT_VARIABLE rc2 OUTPUT_QUIET)
file(READ ${OUT}/par.json par)
file(READ ${OUT}/ser.json ser)
if(NOT rc EQUAL 0 OR NOT rc2 EQUAL 0 OR NOT par STREQUAL ser OR NOT par MATCHES "\"fsms\"")
  message(FATAL_ERROR "mcov merge -j 4 differs from -j 1")
endif()

# msim -compile -cov: the generated code counts the same vectors and the
# kernel records the same state changes.
execute_process(COMMAND ${MSIM} -compile -o cdir ${SV} +define+MORE -cov c.json WORKING_DIRECTORY ${OUT}
                RESULT_VARIABLE rc OUTPUT_QUIET ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT EXISTS ${OUT}/cdir/simv)
  message(FATAL_ERROR "msim -compile -cov exited ${rc}: ${err}")
endif()
execute_process(COMMAND ${MCOV} report b.json -detail WORKING_DIRECTORY ${OUT} OUTPUT_VARIABLE interp)
execute_process(COMMAND ${MCOV} report c.json -detail WORKING_DIRECTORY ${OUT} OUTPUT_VARIABLE comp)
if(NOT interp STREQUAL comp)
  message(FATAL_ERROR "-compile coverage differs from the interpreter's\n--- interpreter\n${interp}--- compiled\n${comp}")
endif()

# One-hot FSMs tested bit by bit, and conditions with a term that && and ||
# must be able to skip (a function call, a system function, a class handle):
# these are counted as they run, and the skipped terms show as "-".
set(OH ${SRC}/tests/cov/fsm_onehot.sv)
execute_process(COMMAND ${MSIM} ${OH} +define+CLS -cov oh.json WORKING_DIRECTORY ${OUT}
                RESULT_VARIABLE rc OUTPUT_VARIABLE run ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT run MATCHES "hits=3 act=1")
  message(FATAL_ERROR "fsm_onehot.sv with -cov exited ${rc}: ${run}${err}")
endif()
execute_process(COMMAND ${MCOV} report oh.json -detail WORKING_DIRECTORY ${OUT} OUTPUT_VARIABLE out)
foreach(want
    "Condition: 53\\.3% \\(8/15 vectors of 5 condition\\(s\\)\\)"
    "FSM: states 80\\.0% \\(4/5\\), transitions 50\\.0% \\(3/6\\) of 2 FSM\\(s\\)"
    "FSM tb\\.u\\.state  \\(module oh, [^\n]*\\)  states 2/3, transitions 1/4\n    state IDLE +1\n    state RUN +1\n    state DONE +Not covered\n    IDLE -> RUN +1\n    RUN -> IDLE +Not covered  \\(reset\\)\n    RUN -> DONE +Not covered\n    DONE -> IDLE +Not covered\n"
    "FSM tb\\.v\\.s  \\(module ohif, [^\n]*\\)  states 2/2, transitions 2/2\n    state s\\[0\\] +2\n    state s\\[1\\] +1\n"
    "p != null && p\\.len > 2\n[^\n]*\n[^\n]*\n[^\n]*\n     1   1   Not covered\n     0   -   1\n     1   0   Not covered"
    "n > 1 && odd\\(n\\)\n[^\n]*\n[^\n]*\n[^\n]*\n     1   1   1\n     0   -   2\n     1   0   1"
    "stop \\|\\| \\$countones\\(vec\\) > 1\n[^\n]*\n[^\n]*\n[^\n]*\n     0   0   2\n     1   -   Not covered\n     0   1   1")
  if(NOT out MATCHES "${want}")
    message(FATAL_ERROR "one-hot / short-circuit: no match for '${want}' in\n${out}")
  endif()
endforeach()
if(out MATCHES "not in the next-state logic")
  message(FATAL_ERROR "a one-hot register cleared and set in one time step was taken for a state change\n${out}")
endif()
# Without the class the design compiles: the same database from simv.
execute_process(COMMAND ${MSIM} ${OH} -cov ohi.json WORKING_DIRECTORY ${OUT} RESULT_VARIABLE rc OUTPUT_QUIET)
execute_process(COMMAND ${MSIM} -compile -o ohdir ${OH} -cov ohc.json WORKING_DIRECTORY ${OUT}
                RESULT_VARIABLE rc2 OUTPUT_QUIET ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT rc2 EQUAL 0 OR NOT EXISTS ${OUT}/ohdir/simv)
  message(FATAL_ERROR "fsm_onehot.sv: msim -compile -cov exited ${rc2}: ${err}")
endif()
execute_process(COMMAND ${MCOV} report ohi.json -detail WORKING_DIRECTORY ${OUT} OUTPUT_VARIABLE interp)
execute_process(COMMAND ${MCOV} report ohc.json -detail WORKING_DIRECTORY ${OUT} OUTPUT_VARIABLE comp)
if(NOT interp STREQUAL comp OR NOT comp MATCHES "odd\\(n\\)")
  message(FATAL_ERROR "fsm_onehot.sv: -compile coverage differs from the interpreter's\n--- interpreter\n${interp}--- compiled\n${comp}")
endif()
