# msim -i / -do as VCS UCLI and the xrun Tcl shell: tests/debug/ucli.do runs
# tests/debug/ucli.sv through a stop at a line, next (over a task call) and
# step, describe, scope / show, get -radix, condition and time stops, Tcl
# (set, foreach, proc, puts), alias, senv, search, a stop with -continue and
# -command, listing, -once, dump -add, run -posedge and run -slot. Then:
# commands that end while stopped at a line run the rest of the simulation,
# and quit at a line stops it.
#   cmake -DMSIM=<msim> -DSRC=<source dir> -DOUT=<dir> -P msim_ucli.cmake
set(SV ${SRC}/tests/debug/ucli.sv)
file(MAKE_DIRECTORY ${OUT})
execute_process(COMMAND ${MSIM} ${SV} -do ${SRC}/tests/debug/ucli.do WORKING_DIRECTORY ${OUT}
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "msim -do exited ${rc}: ${err}\n${out}")
endif()
foreach(want
    "stop 1 set on line ucli.sv:23\n"
    "time 12: stop 1: ucli.sv:23  add\\(3\\);\n"
    "time 12, stopped at ucli.sv:23  add\\(3\\);\n  in initial of tb\n"
    "time 12: next: ucli.sv:24  #200 \\$display"
    "time 15: step: ucli.sv:20  always #5 clk = ~clk;\n"
    "time 15: step: ucli.sv:6  if \\(en\\) q <= q \\+ 1;\n"
    "tb.sum = 32'h4\n"
    "time 18: ran to time 18 \\(next event at 20\\)\n"
    "tb.u1: instance of module counter, declared at ucli.sv:3\n"
    "tb.q: variable logic\\[3:0\\], 4 bits, declared at ucli.sv:10\n"
    "tb.lane\\[1\\].r: variable logic\\[7:0\\], 8 bits, declared at ucli.sv:14\n"
    "no object 'nope'\n"
    "tb.u1\nclk \\(input\\) = 1'h1\nen \\(input\\) = 1'h1\nq \\(output\\) = 4'h1\n"
    "tb.u1.W: parameter logic signed\\[31:0\\] = 4"
    "1\ntb\ntb\n"
    "time 35: stop 2: {q == 3 && en}\n0011\n"
    "time 47: stop 3: time 47\n"
    "high: 2\n"
    "time 52: ran to time 52 \\(next event at 55\\)\ntime 57: ran to time 57"
    "time 67: ran to time 67 \\(next event at 70\\)\n67\n"
    "tb.lane\\[0\\].r\ntb.lane\\[1\\].r\n"
    "time 75: stop 4: tb.clk = 1'h1\nedge q=7\ntime 85: stop 4: tb.clk = 1'h1\nedge q=8\ntime 87: ran to time 87"
    "->   20    always #5 clk = ~clk;\n"
    "time 125: stop 5: tb.u1.q = 4'hc\nno stops\n"
    "u1 \\(instance of counter\\)\nlane\\[0\\] \\(generate block\\)\nlane\\[1\\] \\(generate block\\)\n"
    "unknown command 'bogus'"
    "time 145: tb.clk = 1'h1\ntime 160: slot\ntime 160\nq=4 sum=4\n")
  if(NOT out MATCHES "${want}")
    message(FATAL_ERROR "msim -do: no match for '${want}' in\n${out}")
  endif()
endforeach()
file(READ ${OUT}/ucli.vcd vcd)
if(NOT vcd MATCHES "\\$scope module u1 \\$end\n\\$var wire 1 [^ ]+ clk \\$end" OR vcd MATCHES "lane")
  message(FATAL_ERROR "dump -add u1 -depth 1: wrong ucli.vcd\n${vcd}")
endif()

# Standard input gives the same answers.
execute_process(COMMAND ${MSIM} ${SV} -i INPUT_FILE ${SRC}/tests/debug/ucli.do WORKING_DIRECTORY ${OUT}
                OUTPUT_VARIABLE out2 ERROR_VARIABLE err)
if(NOT out2 STREQUAL out)
  message(FATAL_ERROR "msim -i on standard input differs from -do: ${err}\n${out2}")
endif()

# The commands end while stopped at a line: the run goes on to its end.
file(WRITE ${OUT}/eof.do "stop -line 17\nrun\n")
execute_process(COMMAND ${MSIM} ${SV} -do ${OUT}/eof.do OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT out MATCHES "time 12: stop 1: ucli.sv:17  sum = sum \\+ v;\nq=4 sum=4\n")
  message(FATAL_ERROR "end of commands at a line stop:\n${out}${err}")
endif()
# quit at a line stops the run there.
file(WRITE ${OUT}/quit.do "stop -line 17\nrun\nquit\n")
execute_process(COMMAND ${MSIM} ${SV} -do ${OUT}/quit.do OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT out MATCHES "quit at time 12\n" OR out MATCHES "q=")
  message(FATAL_ERROR "quit at a line stop:\n${out}${err}")
endif()
# Errors: a line with no statement, a bad condition.
file(WRITE ${OUT}/err.do "stop -line 2\nstop -condition {q ==}\nget nope\nquit\n")
execute_process(COMMAND ${MSIM} ${SV} -do ${OUT}/err.do OUTPUT_VARIABLE out ERROR_VARIABLE err)
foreach(want "Error: no statement at .*ucli.sv:2\n" "Error: bad condition 'q =='" "Error: no signal 'nope'\n")
  if(NOT out MATCHES "${want}")
    message(FATAL_ERROR "msim -do errors: no match for '${want}' in\n${out}")
  endif()
endforeach()
