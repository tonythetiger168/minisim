# mdebug -lint FILE: mlint's JSON findings tied to the page's files and to
# the signal each one names (its "signal" field, else a name quoted in its
# text, declared in the same file). tests/debug/lintme.json is a fixed copy
# of mlint -json output plus a waived finding with "signal" and one in a file
# that is not in the design; with MLINT, mlint's own output is read too.
#   cmake -DMDEBUG=<mdebug> [-DMLINT=<mlint>] -DSRC=<source dir> -DOUT=<dir> -P mdebug_lint.cmake
set(SV ${SRC}/tests/debug/lintme.sv)
file(MAKE_DIRECTORY ${OUT})
file(WRITE ${OUT}/q.txt "lint\n")
execute_process(COMMAND ${MDEBUG} ${SV} -top lintme -lint ${SRC}/tests/debug/lintme.json -script ${OUT}/q.txt -json ${OUT}/page.json
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "mdebug -lint exited ${rc}: ${err}")
endif()
foreach(want
    "\"line\":5,\"rule\":\"UNU002\",\"severity\":\"warning\",\"signal\":\"lintme.q\",\"waived\":false"
    "\"line\":8,\"rule\":\"W001\",\"severity\":\"warning\",\"signal\":\"lintme.y\",\"waived\":false"
    "\"rule\":\"UNU001\",\"severity\":\"info\",\"signal\":\"lintme.unused_r\",\"waived\":true"
    "\"file\":\"elsewhere.sv\",\"line\":1,\"rule\":\"STY001\",\"severity\":\"info\",\"signal\":\"\"")
  if(NOT out MATCHES "${want}")
    message(FATAL_ERROR "mdebug script lint: no match for '${want}' in\n${out}")
  endif()
endforeach()
file(READ ${OUT}/page.json page)
if(NOT page MATCHES "\"lint\":\\[\\[0,5,9,\"UNU002\",\"warning\",\"'q' is driven but never read\",0,[0-9]+,\"unused\"\\],")
  message(FATAL_ERROR "mdebug -json: no lint section")
endif()
if(NOT page MATCHES "\\[-1,1,1,\"STY001\"")
  message(FATAL_ERROR "mdebug -json: a finding outside the design should have file -1")
endif()
execute_process(COMMAND ${MDEBUG} ${SV} -top lintme -lint ${SV} RESULT_VARIABLE rc OUTPUT_QUIET ERROR_VARIABLE err)
if(rc EQUAL 0 OR NOT err MATCHES "is not mlint's JSON")
  message(FATAL_ERROR "mdebug -lint on a non-JSON file: rc ${rc}, ${err}")
endif()

if(MLINT)
  execute_process(COMMAND ${MLINT} ${SV} -top lintme -json ${OUT}/live.json OUTPUT_QUIET ERROR_QUIET)
  execute_process(COMMAND ${MDEBUG} ${SV} -top lintme -lint ${OUT}/live.json -script ${OUT}/q.txt OUTPUT_VARIABLE live RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0 OR NOT live MATCHES "\"signal\":\"lintme.q\"")
    message(FATAL_ERROR "mdebug -lint on mlint's own output:\n${live}")
  endif()
endif()
