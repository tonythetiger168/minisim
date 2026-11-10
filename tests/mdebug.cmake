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
# The same trace on any thread count, and from an FST.
set(ref "${out}")
run("-vcd;${OUT}/alu.vcd;-active;tb.sum;-depth;6;-threads;1")
if(NOT out STREQUAL ref)
  message(FATAL_ERROR "-threads 1 gives another trace:\n${out}")
endif()
if(FST)
  execute_process(COMMAND ${MSIM} ${SV} -top tb -fst ${OUT}/alu.fst RESULT_VARIABLE rc OUTPUT_QUIET)
  run("-fst;${OUT}/alu.fst;-active;tb.sum;-depth;6;-threads;3")
  if(NOT out STREQUAL ref)
    message(FATAL_ERROR "the FST gives another trace than the VCD:\n${out}${err}")
  endif()
endif()

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
foreach(w "<title>mdebug</title>" "\"path\":\"tb.u1.y\"" "always_ff @(posedge clk or posedge rst)" "\"waves\":{" "module alu" "\"cases\":[{" "[\"case\",0,1]" "[\"b\",\"Add\",")
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

# Tasks, call outputs, force and a net's declaration assignment are drivers.
set(SV ${SRC}/tests/debug/task.sv)
execute_process(COMMAND ${MSIM} ${SV} -top tk -vcd ${OUT}/task.vcd RESULT_VARIABLE rc OUTPUT_VARIABLE out)
if(NOT rc EQUAL 0 OR NOT out MATCHES "w=5 q=7 f=170")
  message(FATAL_ERROR "msim task.sv: ${rc}\n${out}")
endif()
function(runtk args)
  execute_process(COMMAND ${MDEBUG} ${SV} -top tk ${args}
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  set(out "${out}" PARENT_SCOPE)
  set(err "${err}" PARENT_SCOPE)
endfunction()
runtk("-drivers;tk.q;-drivers;tk.w;-drivers;tk.f;-drivers;tk.n;-loads;tk.a")
want("task.sv:10: q = v  [task set_q]  when v != 0"
     "task.sv:13: bump(a, w)  [always @posedge clk, call]"
     "drivers of tk.f: 2" "task.sv:18: f = 8'haa  [initial, force]"
     "drivers of tk.n: 1" "loads of tk.a: 3" "-> tk.bump.x" "-> tk.w")
runtk("-trace;tk.q")
want("tk.set_q.v" "task.sv:17: set_q(8'd7)  [initial, call]")
runtk("-vcd;${OUT}/task.vcd;-active;tk.f;-at;40")
want("tk.f = 8'haa @40  (since 32)" "f = 8'haa  [initial, force]")
if(out MATCHES "f = n")
  message(FATAL_ERROR "the forced value was traced to the overridden driver:\n${out}")
endif()
runtk("-vcd;${OUT}/task.vcd;-active;tk.w;-at;30")
want("tk.w = 8'h05 @30  (since 15)" "bump(a, w)" "a = 8'd3  [initial]")

# The block view: instances, their port connections, and what feeds what.
set(SV ${SRC}/tests/debug/alu.sv)
run("-blocks;tb")
want("blocks of tb: 2" "u0 (alu)" "in  op <- tb.op" "in  op <- 2'd2" "in  b <- 8'h0f"
     "out y -> tb.y1" "u0.y -> u1.a  (tb.y0)")
run("-blocks;u0")
want("blocks of tb.u0: 0")
run("-blocks;nosuch")
if(rc EQUAL 0 OR NOT err MATCHES "no instance 'nosuch'")
  message(FATAL_ERROR "expected an error for an unknown instance: ${rc}\n${out}${err}")
endif()
run("-html;${OUT}/blk.html")
file(READ ${OUT}/blk.html page)
if(NOT page MATCHES "function blocks\\(\\)")
  message(FATAL_ERROR "the page has no block view")
endif()

# Struct fields: a condition on a field is evaluated, each field has its own
# driver, and a whole assignment the fields overwrite is not reported.
set(SV ${SRC}/tests/debug/struct.sv)
execute_process(COMMAND ${MSIM} ${SV} -top st -vcd ${OUT}/struct.vcd RESULT_VARIABLE rc OUTPUT_VARIABLE out)
if(NOT rc EQUAL 0 OR NOT out MATCHES "s=83 t=0")
  message(FATAL_ERROR "msim struct.sv: ${rc}\n${out}")
endif()
execute_process(COMMAND ${MDEBUG} ${SV} -top st -vcd ${OUT}/struct.vcd -active st.s -at 50 -depth 2
                        -active st.t -at 50 -depth 2 -json ${OUT}/struct.json
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
want("struct.sv:13: s = {p.hi, p.lo}  [always_comb]  when p.lo == 4'd3"
     "p.hi = q[3:0]" "p.lo = 4'd3" "q <= q + 8'd4"
     "t = r.b ? r.a : 4'd0" "st.r = 5'h09 @45" "r.a = p.hi" "r.b = idx[0]")
if(out MATCHES "\\?\\?|p = '0|s = 0")
  message(FATAL_ERROR "struct trace: an undecided or overwritten driver was reported:\n${out}")
endif()
file(READ ${OUT}/struct.json page)
if(NOT page MATCHES "\"fld\"" OR NOT page MATCHES "\"lo\":4,\"w\":4")
  message(FATAL_ERROR "the database has no field expressions or bit ranges")
endif()
