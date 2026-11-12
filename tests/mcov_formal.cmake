# mcov exclude -formal: what mformal proves unreachable becomes an exclusion.
#   - tests/cov/unreach.sv: mformal -app toggle proves dut.dead stuck at 0;
#     the exclusion takes it out of toggle coverage, the found scope is tb.dut.
#   - a target of another app that names its exclusion ("mcov": "state ..."),
#     on tests/cov/fsm.sv: kept when not covered, reported when covered.
#   cmake -DPY=<python3> -DMSIM=<msim> -DMFORMAL=<mformal> -DSRC=<source dir> -DOUT=<dir> -P mcov_formal.cmake
file(REMOVE_RECURSE ${OUT})
file(MAKE_DIRECTORY ${OUT})
set(MCOV ${PY} ${SRC}/tools/mcov.py)
execute_process(COMMAND ${MSIM} ${SRC}/tests/cov/unreach.sv -top unreach_tb -cov u.json WORKING_DIRECTORY ${OUT}
                RESULT_VARIABLE rc OUTPUT_QUIET ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "msim -cov exited ${rc}: ${err}")
endif()
execute_process(COMMAND ${MFORMAL} ${SRC}/tests/cov/unreach.sv -top unreach -app toggle -json f.json -threads 2
                WORKING_DIRECTORY ${OUT} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "mformal exited ${rc}: ${out}${err}")
endif()
execute_process(COMMAND ${MCOV} exclude u.json -formal f.json -o u.el WORKING_DIRECTORY ${OUT} OUTPUT_VARIABLE out)
file(READ ${OUT}/u.el el)
if(NOT el MATCHES "\ntoggle unreach_tb\\.dut\\.dead  -- unreachable: mformal \\(f\\.json")
  message(FATAL_ERROR "no exclusion for dut.dead:\n${out}\n${el}")
endif()
if(el MATCHES "cnt")
  message(FATAL_ERROR "a reachable bit was excluded:\n${el}")
endif()
execute_process(COMMAND ${MCOV} report u.json WORKING_DIRECTORY ${OUT} OUTPUT_VARIABLE before)
execute_process(COMMAND ${MCOV} report u.json -elfile u.el -el-strict WORKING_DIRECTORY ${OUT} RESULT_VARIABLE rc OUTPUT_VARIABLE after)
string(REGEX MATCH "Toggle: [^\n]*\\(([0-9]+)/([0-9]+) bits" m "${before}")
set(h ${CMAKE_MATCH_1})
math(EXPR t "${CMAKE_MATCH_2} - 1")
if(NOT rc EQUAL 0 OR NOT after MATCHES "Toggle: [^\n]*\\(${h}/${t} bits")
  message(FATAL_ERROR "the exclusion should remove one bit and pass -el-strict (${rc}):\n${before}\n${after}")
endif()
execute_process(COMMAND ${MCOV} exclude u.json -formal f.json -formal-scope unreach_tb.dut -o s.el WORKING_DIRECTORY ${OUT} OUTPUT_QUIET)
file(READ ${OUT}/s.el s)
if(NOT s STREQUAL el)
  message(FATAL_ERROR "-formal-scope gives another file:\n${s}\n---\n${el}")
endif()

# The form other apps use.
execute_process(COMMAND ${MSIM} ${SRC}/tests/cov/fsm.sv -cov a.json WORKING_DIRECTORY ${OUT} RESULT_VARIABLE rc OUTPUT_QUIET)
file(WRITE ${OUT}/s.json [=[{"tool": "mformal", "properties": [
  {"name": "ctrl.state==FAIL", "kind": "cover", "result": "unreachable", "mcov": "state ctrl.state FAIL"},
  {"name": "ctrl.state==RUN", "kind": "cover", "result": "unreachable", "mcov": "state ctrl.state RUN"},
  {"name": "ctrl.state==WAIT", "kind": "cover", "result": "covered", "mcov": "state ctrl.state WAIT"}]}
]=])
execute_process(COMMAND ${MCOV} exclude a.json -formal s.json -formal-scope tb.u -o f.el WORKING_DIRECTORY ${OUT} OUTPUT_VARIABLE out)
file(READ ${OUT}/f.el fel)
if(NOT fel MATCHES "\nstate tb\\.u\\.state FAIL  -- unreachable: mformal" OR fel MATCHES "RUN|WAIT"
   OR NOT out MATCHES "state tb\\.u\\.state RUN: proven unreachable but the tests covered it")
  message(FATAL_ERROR "state targets:\n${out}\n${fel}")
endif()
# Without -formal-scope, the instance of mformal's top module is found even
# when another FSM has the same register name (tb.v.st is module one, tb.w.ph ifs).
file(WRITE ${OUT}/one.json [=[{"tool": "mformal", "properties": [
  {"name": "one.st==S2", "kind": "cover", "result": "unreachable", "mcov": "trans one.st S2->S0"}]}
]=])
execute_process(COMMAND ${MCOV} exclude a.json -formal one.json -o one.el WORKING_DIRECTORY ${OUT} OUTPUT_VARIABLE out)
file(READ ${OUT}/one.el oel)
if(NOT oel MATCHES "\ntrans tb\\.v\\.st S2->S0  -- unreachable")
  message(FATAL_ERROR "scope from the top module:\n${out}\n${oel}")
endif()
file(WRITE ${OUT}/bad.json "{\"tool\": \"other\"}")
execute_process(COMMAND ${MCOV} exclude a.json -formal bad.json -o x.el WORKING_DIRECTORY ${OUT} RESULT_VARIABLE rc OUTPUT_QUIET ERROR_VARIABLE err)
if(rc EQUAL 0 OR NOT err MATCHES "not an mformal -json result")
  message(FATAL_ERROR "a file that is not mformal's should be an error: ${rc} ${err}")
endif()
message(STATUS "mcov -formal: ok")
