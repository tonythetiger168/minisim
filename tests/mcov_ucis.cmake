# mcov and UCIS XML (Accellera UCIS 1.0, the XML interchange format): a
# database named .xml is written and read as UCIS.
#   - msim databases written as UCIS and read back report the same, per
#     instance and in detail; writing what was read gives the same file.
#   - tests/cov/ucis_other.xml, in the shape another simulator writes (blocks,
#     a case statement, SV literal state values, a cross), reads as expected.
#   cmake -DPY=<python3> -DMSIM=<msim> -DSRC=<source dir> -DOUT=<dir> [-DXMLLINT=<xmllint> -DXSD=<ucis.xsd>] -P mcov_ucis.cmake
file(REMOVE_RECURSE ${OUT})
file(MAKE_DIRECTORY ${OUT})
set(MCOV ${CMAKE_COMMAND} -E env SOURCE_DATE_EPOCH=0 ${PY} ${SRC}/tools/mcov.py)
foreach(t fsm counter inst)
  execute_process(COMMAND ${MSIM} ${SRC}/tests/cov/${t}.sv -cov ${t}.json WORKING_DIRECTORY ${OUT}
                  RESULT_VARIABLE rc OUTPUT_QUIET ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "msim ${t}.sv -cov exited ${rc}: ${err}")
  endif()
endforeach()
function(mcov)
  execute_process(COMMAND ${MCOV} ${ARGN} WORKING_DIRECTORY ${OUT} RESULT_VARIABLE rc OUTPUT_VARIABLE o ERROR_VARIABLE e)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "mcov ${ARGN} exited ${rc}:\n${o}${e}")
  endif()
  set(out "${o}" PARENT_SCOPE)
endfunction()

mcov(merge fsm.json counter.json inst.json -o all.json)
mcov(merge fsm.json counter.json inst.json -o all.xml)
file(READ ${OUT}/all.xml x)
foreach(want "^<\\?xml version=\"1.0\"" "<ucis:UCIS xmlns:ucis=\"http://www.ucis.org/ucis/1.0\" ucisVersion=\"1.0\""
             "<sourceFiles fileName=\"[^\"]*tests/cov/fsm.sv\" id=\"[0-9]+\"/>" "<historyNodes historyNodeId=\"0\" logicalName="
             "<instanceCoverages name=\"u1\" key=\"[0-9]+\" instanceId=\"[0-9]+\" parentInstanceId=\"[0-9]+\" moduleName=\"mid\">"
             "<toggleCoverage" "<blockCoverage" "<conditionCoverage" "<branchCoverage" "<fsmCoverage"
             "<assertionCoverage" "<covergroupCoverage" "<stateTransition><state>IDLE</state><state>RUN</state>"
             "<subExpr>a</subExpr><subExpr>b</subExpr>" "</ucis:UCIS>\n$")
  if(NOT x MATCHES "${want}")
    message(FATAL_ERROR "all.xml has no '${want}'")
  endif()
endforeach()
if(XMLLINT AND XSD)
  # The schema has no target namespace: the root is checked without its prefix.
  string(REGEX REPLACE "<ucis:UCIS xmlns:ucis=\"[^\"]*\"" "<UCIS" y "${x}")
  string(REPLACE "</ucis:UCIS>" "</UCIS>" y "${y}")
  file(WRITE ${OUT}/plain.xml "${y}")
  execute_process(COMMAND ${XMLLINT} --noout --schema ${XSD} plain.xml WORKING_DIRECTORY ${OUT}
                  RESULT_VARIABLE rc ERROR_VARIABLE e)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "all.xml is not valid UCIS:\n${e}")
  endif()
endif()

# The same numbers from either form.
foreach(args "" "-detail;-instances" "-uncovered;-fsm-reset-filter")
  mcov(report all.json ${args})
  set(a "${out}")
  mcov(report all.xml ${args})
  if(NOT a STREQUAL out)
    message(FATAL_ERROR "report ${args} differs between JSON and UCIS:\n${a}\n---\n${out}")
  endif()
endforeach()
# Read back and written again: the same file.
mcov(merge all.xml -o rt.json)
mcov(merge rt.json -o rt.xml)
file(READ ${OUT}/rt.xml r)
if(NOT r STREQUAL x)
  message(FATAL_ERROR "UCIS -> JSON -> UCIS changed the file")
endif()
# UCIS and JSON databases merge with each other, and rank takes either.
mcov(merge all.xml fsm.json -o mixed.json)
mcov(report mixed.json)
if(NOT out MATCHES "4 run\\(s\\)")
  message(FATAL_ERROR "a UCIS and a JSON database do not merge:\n${out}")
endif()
mcov(merge fsm.json -o fsm.xml)
mcov(rank fsm.xml counter.json)
if(NOT out MATCHES "fsm.sv|fsm.json|counter")
  message(FATAL_ERROR "rank of a UCIS database:\n${out}")
endif()

# Another tool's file.
mcov(report ${SRC}/tests/cov/ucis_other.xml -detail -instances)
foreach(want "rtl/alu.sv +100.0% +2/2 +83.3% +5/6" "rtl/top.sv +50.0% +1/2"
             "Toggle: 50.0% \\(1/2 bits" "Condition: 66.7% \\(2/3 vectors of 1"
             "FSM: states 66.7% \\(2/3\\), transitions 66.7% \\(2/3\\)" "top.u_alu +alu "
             "IDLE -> BUSY +2" "BUSY -> DONE +Not covered" "top.u_alu.cg_op +50.0% +1/2 +bins"
             "Assertions: 50.0% \\(1/2 passed or hit, 1 never")
  if(NOT out MATCHES "${want}")
    message(FATAL_ERROR "ucis_other.xml: no '${want}' in\n${out}")
  endif()
endforeach()
file(WRITE ${OUT}/bad.xml "<ucis:UCIS xmlns:ucis=\"http://www.ucis.org/ucis/1.0\"><sourceFiles")
execute_process(COMMAND ${MCOV} report bad.xml WORKING_DIRECTORY ${OUT} RESULT_VARIABLE rc OUTPUT_QUIET ERROR_VARIABLE e)
if(rc EQUAL 0 OR NOT e MATCHES "not well-formed XML")
  message(FATAL_ERROR "a broken XML file should be an error: ${rc} ${e}")
endif()
message(STATUS "mcov UCIS: ok")
