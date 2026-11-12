# Interactive simulation (msim -i / -do, as VCS UCLI): tests/debug/interactive.do
# drives tests/debug/interactive.sv through stops, step, run T, force,
# release, deposit, and printing of class handles and their properties.
# The same commands on standard input give the same answers, and a run
# without -i still prints what the design prints.
#   cmake -DMSIM=<msim> -DSRC=<source dir> -P msim_interactive.cmake
set(SV ${SRC}/tests/debug/interactive.sv)
set(DO ${SRC}/tests/debug/interactive.do)
execute_process(COMMAND ${MSIM} ${SV} -do ${DO} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "msim -do exited ${rc}: ${err}\n${out}")
endif()
foreach(want
    "tb.cnt = 8'h0\n"
    "tb.none = null\n"
    "stop 1 set on tb.cnt\n"
    "time 5: stop 2: tb.clk = 1'h1\n"
    "tb.head = item @[0-9]+ { id = 32'h1, data = 8'h11, tag = \"it1\", next = big @[0-9]+ }\n"
    "tb.head.next = big @[0-9]+ { id = 32'h2, data = 8'h12, tag = \"it2\", next = null, flag = 1'h1 }\n"
    "tb.head.next.id = 32'h2\n"
    "tb.b.flag = 1'h1\n"
    "no property 'zz' in tb.head\n"
    "time 35: stop 1: tb.cnt = 8'h3\n"
    "time 45\n"
    "tb.en = 1'h0  \\(forced\\)\n"
    "time 75: ran to time 75 \\(next event at 80\\)\n"
    "tb.cnt = 8'h4\n"
    "tb.en released\n"
    "time 95: ran to time 95 \\(next event at 100\\)\n"
    "tb.cnt = 8'h40\n"
    "unknown command 'bogus'"
    "no signal 'tb.nope'\n"
    "cnt=64\n"
    "simulation ended at time 112\n")
  if(NOT out MATCHES "${want}")
    message(FATAL_ERROR "msim -do: no match for '${want}' in\n${out}")
  endif()
endforeach()

execute_process(COMMAND ${MSIM} ${SV} -i INPUT_FILE ${DO} RESULT_VARIABLE rc OUTPUT_VARIABLE out2 ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT out2 STREQUAL out)
  message(FATAL_ERROR "msim -i on standard input differs from -do (${rc}): ${err}\n${out2}")
endif()

# quit stops at once; no commands runs to the end.
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/quit.do "run 20\nquit\n")
execute_process(COMMAND ${MSIM} ${SV} -do ${CMAKE_CURRENT_BINARY_DIR}/quit.do OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(out MATCHES "cnt=")
  message(FATAL_ERROR "quit did not stop the run:\n${out}")
endif()
execute_process(COMMAND ${MSIM} ${SV} -i INPUT_FILE /dev/null OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT out MATCHES "cnt=10\n")
  message(FATAL_ERROR "msim -i with no commands did not run to the end:\n${out}")
endif()
execute_process(COMMAND ${MSIM} ${SV} OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT out STREQUAL "cnt=10\n")
  message(FATAL_ERROR "msim without -i changed:\n${out}")
endif()
