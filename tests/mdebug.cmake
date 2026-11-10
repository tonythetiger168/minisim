# mdebug self-test on tests/debug/alu.sv: drivers, loads, the fan-in cone, the
# active-driver trace against a VCD msim writes, and the viewer page.
#   cmake -DMDEBUG=<mdebug> -DMSIM=<msim> -DSRC=<source dir> -DOUT=<dir> -P mdebug.cmake
file(MAKE_DIRECTORY ${OUT})
set(SV ${SRC}/tests/debug/alu.sv)
execute_process(COMMAND ${MSIM} ${SV} -top tb -vcd ${OUT}/alu.vcd RESULT_VARIABLE rc OUTPUT_VARIABLE out)
if(NOT rc EQUAL 0 OR NOT out MATCHES "y0=17 y1=15 sum=32")
  message(FATAL_ERROR "msim alu.sv: ${rc}\n${out}")
endif()

function(run args)
  execute_process(COMMAND ${MDEBUG} ${SV} -top tb ${args}
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  set(out "${out}" PARENT_SCOPE)
  set(err "${err}" PARENT_SCOPE)
  set(rc "${rc}" PARENT_SCOPE)
endfunction()
function(want)
  foreach(w ${ARGN})
    string(FIND "${out}" "${w}" pos)
    if(pos EQUAL -1)
      message(FATAL_ERROR "missing: ${w}\n${out}${err}")
    endif()
  endforeach()
endfunction()

# Drivers: each case arm with its condition; the flop with reset and enable.
# u0 and u1 are separate signals.
run("-drivers;tb.u0.r;-drivers;u1.y")
want("drivers of tb.u0.r: 4"
     "alu.sv:12: r = a + b  [always_comb]  when op == 2'd0"
     "alu.sv:15: r = a ^ b  [always_comb]  when op: default"
     "drivers of tb.u1.y: 2"
     "alu.sv:19: y <= '0  [always_ff @(posedge clk or posedge rst)]  when rst"
     "alu.sv:20: y <= r  [always_ff @(posedge clk or posedge rst)]  when !(rst) && en")

# Loads: through a port into u1, and the adder; a condition is marked.
run("-loads;tb.y0;-loads;tb.u0.en")
want("loads of tb.y0: 2" "-> tb.u1.a" "alu.sv:29: sum = y0 + y1  [assign]  -> tb.sum"
     "-> tb.u0.y  (control)")

# The cone crosses the port and reaches the testbench.
run("-trace;tb.y0;-depth;4")
want("tb.u0.y" "tb.u0.r" "tb.u0.op" "tb.op")

# Active driver: at 30 the add arm ran (op=0), at 40 the subtract arm, at the
# end the xor arm; the flop is sampled before its clock edge.
run("-vcd;${OUT}/alu.vcd;-active;tb.u0.y;-at;30")
want("tb.u0.y = 8'h19 @30  (since 25)" "y <= r" "with rst=0, en=1 @24"
     "r = a + b  [always_comb]  when op == 2'd0" "tb.u0.a = 8'h14 @22")
if(out MATCHES "r = a - b|y <= '0")
  message(FATAL_ERROR "an inactive driver was reported:\n${out}")
endif()
run("-vcd;${OUT}/alu.vcd;-active;tb.u0.y;-at;40")
want("tb.u0.y = 8'h0f @40" "r = a - b  [always_comb]  when op == 2'd1" "tb.u0.op = 2'h1 @32")
run("-vcd;${OUT}/alu.vcd;-active;tb.sum;-depth;6")
want("tb.sum = 8'h20 @70  (since 45)" "r = a ^ b  [always_comb]  when op: default"
     "r = a & b  [always_comb]  when op == 2'd2" "op = 2'd3  [initial]")
# In reset, the reset branch is the driver.
run("-vcd;${OUT}/alu.vcd;-active;tb.u0.y;-at;10")
want("tb.u0.y = 8'h00 @10" "y <= '0" "with rst=1")

# An unknown signal is an error; -active without a waveform too.
run("-drivers;tb.nosuch")
if(rc EQUAL 0 OR NOT err MATCHES "no signal 'tb.nosuch'")
  message(FATAL_ERROR "expected an error for an unknown signal: ${rc}\n${out}${err}")
endif()
run("-active;tb.sum")
if(rc EQUAL 0 OR NOT err MATCHES "needs -vcd")
  message(FATAL_ERROR "expected -active to ask for -vcd: ${rc}\n${out}${err}")
endif()

# The viewer: one page with the database, the sources and the waves in it.
run("-vcd;${OUT}/alu.vcd;-html;${OUT}/alu.html;-json;${OUT}/alu.json")
want("3 instances")
file(READ ${OUT}/alu.html html)
foreach(w "<title>mdebug</title>" "\"path\":\"tb.u1.y\"" "always_ff @(posedge clk or posedge rst)" "\"waves\":{" "module alu")
  string(FIND "${html}" "${w}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "the viewer page lacks: ${w}")
  endif()
endforeach()
if(html MATCHES "MDEBUG_DATA")
  message(FATAL_ERROR "the database was not spliced into the page")
endif()
file(READ ${OUT}/alu.json json)
string(JSON n LENGTH "${json}" signals)
if(NOT n EQUAL 25)
  message(FATAL_ERROR "expected 25 signals in the database, got ${n}")
endif()
