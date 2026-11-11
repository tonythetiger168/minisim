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
# A read of one field follows that field only: its value, the last time it
# changed and its driver, not the assignments to the other fields.
execute_process(COMMAND ${MDEBUG} ${SV} -top st -vcd ${OUT}/struct.vcd -active st.u -at 50 -depth 3
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
want("u = p.hi + 4'd1" "st.p.hi = 4'h4 @45" "p.hi = q[3:0]" "st.q[3:0] = 4'h4 @45")
if(out MATCHES "p.lo = 4'd3|p = '0")
  message(FATAL_ERROR "field trace: a driver of another field was reported:\n${out}")
endif()
if(NOT page MATCHES "\"part\":{\"[0-9]+\":[[]4,4,\"[.]hi\"[]]}")
  message(FATAL_ERROR "the database does not say which field a driver reads")
endif()

# Memories: msim -vcd-arrays writes the elements of small arrays, and the
# active trace goes from the read to the element to the write. The default
# waveform has no elements.
set(SV ${SRC}/tests/debug/mem.sv)
execute_process(COMMAND ${MSIM} ${SV} -top mm -vcd ${OUT}/mem0.vcd RESULT_VARIABLE rc OUTPUT_VARIABLE out)
file(READ ${OUT}/mem0.vcd dflt)
if(NOT rc EQUAL 0 OR dflt MATCHES "mem\\[")
  message(FATAL_ERROR "msim mem.sv: array elements in the default VCD (${rc})")
endif()
execute_process(COMMAND ${MSIM} ${SV} -top mm -vcd ${OUT}/mem.vcd -vcd-arrays 8 RESULT_VARIABLE rc OUTPUT_VARIABLE out)
file(READ ${OUT}/mem.vcd arr)
if(NOT rc EQUAL 0 OR NOT arr MATCHES "reg 8 [^ ]+ mem\\[3\\]" OR arr MATCHES "big\\[")
  message(FATAL_ERROR "msim -vcd-arrays 8: want mem[0..3] and not big[] (${rc})")
endif()
execute_process(COMMAND ${MDEBUG} ${SV} -top mm -vcd ${OUT}/mem.vcd -active mm.q -at 50 -depth 2
                        -active mm.mem[1] -at 50 -depth 2
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
want("q = mem[2]" "mm.mem[2] = 8'h0c @25" "mem[idx] <= {6'd0, idx} + 8'd10" "with idx=2'h2 @24"
     "mm.mem[1] = 8'h0b @50  (since 15)" "with idx=2'h1 @14")
set(ref "${out}")
execute_process(COMMAND ${MDEBUG} ${SV} -top mm -vcd ${OUT}/mem0.vcd -active mm.q -at 50
                OUTPUT_VARIABLE out ERROR_VARIABLE err)
want("mm.mem[2]  (not in the waveform: run msim with -vcd-arrays)")
if(FST)
  execute_process(COMMAND ${MSIM} ${SV} -top mm -fst ${OUT}/mem.fst -vcd-arrays 8 RESULT_VARIABLE rc OUTPUT_QUIET)
  execute_process(COMMAND ${MDEBUG} ${SV} -top mm -fst ${OUT}/mem.fst -active mm.q -at 50 -depth 2
                          -active mm.mem[1] -at 50 -depth 2 OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT out STREQUAL ref)
    message(FATAL_ERROR "memory trace from the FST differs:\n${out}${err}")
  endif()
endif()

# State machines: the shared extractor's states and transitions with their
# lines; with a waveform, which transitions ran, the state at a time and the
# states never entered. The page gets the same data.
set(SV ${SRC}/tests/cov/fsm.sv)
execute_process(COMMAND ${MSIM} ${SV} -top tb -vcd ${OUT}/fsm.vcd RESULT_VARIABLE rc OUTPUT_VARIABLE out)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "msim fsm.sv: ${rc}\n${out}")
endif()
execute_process(COMMAND ${MDEBUG} ${SV} -top tb -fsm RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
want("tb.u.state  (ctrl, 2 bits, 4 states)" "states: IDLE=0(reset) RUN=1 WAIT=2 FAIL=3" "RUN -> FAIL  " "fsm.sv:16"
     "tb.v.st  (one, 2 bits, 3 states)" "tb.w.ph  (ifs, 2 bits, 3 states)")
if(out MATCHES "taken|cnt")
  message(FATAL_ERROR "-fsm without a waveform reported run data, or a counter as a state machine:\n${out}")
endif()
execute_process(COMMAND ${MDEBUG} ${SV} -top tb -vcd ${OUT}/fsm.vcd -fsm tb.u -at 40 -json ${OUT}/fsm.json
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
want("IDLE -> RUN  " "taken 1 time, first @25" "fsm.sv:16  never taken" "state FAIL never entered"
     "@40: WAIT  (since 35)  after IDLE@0 RUN@25")
if(NOT rc EQUAL 0 OR out MATCHES "tb.v.st")
  message(FATAL_ERROR "-fsm tb.u: rc ${rc}, or a machine of another instance was listed:\n${out}")
endif()
file(READ ${OUT}/fsm.json page)
if(NOT page MATCHES "\"fsms\":\\[{\"sig\":[0-9]+,\"states\":\\[\\[0,\"IDLE\",1\\],\\[1,\"RUN\",0\\]")
  message(FATAL_ERROR "the database has no state machines")
endif()
execute_process(COMMAND ${MDEBUG} ${SV} -top tb -fsm tb.nothing RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(rc EQUAL 0)
  message(FATAL_ERROR "-fsm of a name with no state machine passed")
endif()

# The script interface: one JSON line per command, errors as answers, and
# the exit status says a command failed.
file(WRITE ${OUT}/q.txt "drivers tb.u.state\n# a comment\nvalue tb.u.state 40\nchanges tb.u.state 0 40\nfsm tb.u 40\nactive tb.busy 40 1\nend\n")
execute_process(COMMAND ${MDEBUG} ${SV} -top tb -vcd ${OUT}/fsm.vcd -script ${OUT}/q.txt
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
want("{\"cmd\":\"drivers tb.u.state\",\"ok\":true,\"signal\":\"tb.u.state\",\"drivers\":"
     "\"line\":10,\"text\":\"state \\u003c= IDLE\"" "\"when\":\"!rst_n\""
     "\"time\":40,\"value\":\"2'h2\",\"bits\":\"10\",\"since\":35}"
     "\"changes\":[[0,\"00\"],[25,\"01\"],[35,\"10\"]],\"more\":false}"
     "\"  @40: WAIT  (since 35)  after IDLE@0 RUN@25\"" "\"tb.busy = 1 @40  (since 25)\"" "\"end\":70,")
if(NOT rc EQUAL 0 OR NOT "${err}" STREQUAL "")
  message(FATAL_ERROR "-script: rc ${rc}, stderr: ${err}\n${out}")
endif()
file(WRITE ${OUT}/q.txt "value nope\nbogus\nend\n")
execute_process(COMMAND ${MDEBUG} ${SV} -top tb -vcd ${OUT}/fsm.vcd -script ${OUT}/q.txt
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
want("{\"cmd\":\"value nope\",\"ok\":false,\"error\":\"no signal 'nope' in the design\"}"
     "\"ok\":false,\"error\":\"unknown command 'bogus' (help lists them)\"}" "\"end\":70,")
if(rc EQUAL 0)
  message(FATAL_ERROR "-script with failing commands exited 0")
endif()
# The Python interface on top of it.
if(PY)
  file(WRITE ${OUT}/api.py "import sys\nsys.path.insert(0, sys.argv[1])\nfrom mdebug_api import Mdebug, MdebugError\n"
       "with Mdebug([sys.argv[3], '-top', 'tb'], wave=sys.argv[4], mdebug=sys.argv[2]) as d:\n"
       "    assert d.value('tb.u.state', 40)['value'] == \"2'h2\"\n"
       "    assert [x['line'] for x in d.drivers('tb.u.state')] == [10, 11]\n"
       "    assert d.changes('tb.u.state', 0, 40) == [[0, '00'], [25, '01'], [35, '10']]\n"
       "    assert d.end() == 70 and any('WAIT' in l for l in d.fsm('tb.u', 40))\n"
       "    assert d.active('tb.busy', at=40, depth=1)[0].startswith('tb.busy = 1 @40')\n"
       "    assert len(d.signals('tb.u')) == 8 and any(s['path'] == 'tb.u' for s in d.scopes())\n"
       "    try:\n        d.value('nope')\n        sys.exit('no error for a missing signal')\n"
       "    except MdebugError as e:\n        assert 'nope' in str(e)\n"
       "    assert d.end() == 70\nprint('api ok')\n")
  execute_process(COMMAND ${PY} ${OUT}/api.py ${SRC}/tools ${MDEBUG} ${SV} ${OUT}/fsm.vcd
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT rc EQUAL 0 OR NOT out MATCHES "api ok")
    message(FATAL_ERROR "mdebug_api.py: ${rc}\n${out}${err}")
  endif()
endif()

# Assertions: the list, and with the simulation log and the waveform each
# failure with what the assertion sampled at the clock ticks before it and
# what drove each signal.
set(SV ${SRC}/tests/debug/assert.sv)
execute_process(COMMAND ${MSIM} ${SV} -top tb -vcd ${OUT}/assert.vcd RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out2)
file(WRITE ${OUT}/assert.log "${out}${out2}")
if(NOT "${out}${out2}" MATCHES "tb.u.a_gnt: at time 55")
  message(FATAL_ERROR "msim assert.sv: no failure of a_gnt at 55 (${rc})\n${out}${out2}")
endif()
execute_process(COMMAND ${MDEBUG} ${SV} -top tb -assert RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
want("tb.u.a_gnt  assert property  " "assert.sv:5" "a_gnt: assert property (@(posedge clk) disable iff (rst) req |=> gnt);"
     "tb.u.a_onehot  assert property  " "tb.u.a_imm  assert  ")
if(out MATCHES "failed|no failure")
  message(FATAL_ERROR "-assert without a log reported results:\n${out}")
endif()
execute_process(COMMAND ${MDEBUG} ${SV} -top tb -vcd ${OUT}/assert.vcd -log ${OUT}/assert.log -assert tb.u.a_gnt -json ${OUT}/assert.json
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
want("failed 1 time: @55" "failure @55, values sampled before each clock tick:"
     "@25       @35       @45       @55"
     "tb.u.req         1         0         1         0"
     "tb.u.gnt         0         1         0         0"
     "tb.u.gnt = 0 @54  (since 35)" "gnt <= rst ? 1'b0 : req && !hold  [always_ff @posedge clk]")
if(NOT rc EQUAL 0 OR out MATCHES "a_onehot")
  message(FATAL_ERROR "-assert tb.u.a_gnt: rc ${rc}, or another assertion was listed:\n${out}")
endif()
file(READ ${OUT}/assert.json page)
if(NOT page MATCHES "\"name\":\"tb.u.a_gnt\",\"kind\":\"assert property\"" OR NOT page MATCHES "\"fails\":\\[55\\]")
  message(FATAL_ERROR "the database has no assertions or failures")
endif()
execute_process(COMMAND ${MDEBUG} ${SV} -top tb -vcd ${OUT}/assert.vcd -log ${OUT}/assert.log -assert tb.u.a_imm
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
want("tb.u.a_imm  assert  " "no failure in the log")

# Coverage: with msim's coverage database, driver lines say how often their
# line ran, -uncovered lists what never ran or never toggled, and the page's
# database has the hits per line.
set(SV ${SRC}/tests/cov/fsm.sv)
execute_process(COMMAND ${MSIM} ${SV} -top tb -cov ${OUT}/cov.json RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT EXISTS ${OUT}/cov.json)
  message(FATAL_ERROR "msim -cov: ${rc}\n${out}${err}")
endif()
execute_process(COMMAND ${MDEBUG} ${SV} -top tb -cov ${OUT}/cov.json -drivers tb.u.next -uncovered tb.u -json ${OUT}/covdb.json
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
want("next = RUN  [always_comb]  when state == IDLE && go  (line ran 3 times)"
     "next = IDLE  [always_comb]  when state == FAIL  (line never ran)"
     "assignments on lines that never ran under tb.u:" "signals that never toggled fully:"
     "tb.u.rst_n  (never fell)" "tb.u.err  (never changed)")
if(NOT rc EQUAL 0 OR out MATCHES "tb.v.")
  message(FATAL_ERROR "-uncovered tb.u: rc ${rc}, or another instance was listed:\n${out}${err}")
endif()
file(READ ${OUT}/covdb.json page)
if(NOT page MATCHES "\"cov\":{\"lines\":{\"0\":{\"9\":8" OR NOT page MATCHES "\"19\":0" OR NOT page MATCHES "\"stuck\":{\"[0-9]+\":\"never")
  message(FATAL_ERROR "the database has no coverage section")
endif()
execute_process(COMMAND ${MDEBUG} ${SV} -top tb -uncovered RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(rc EQUAL 0 OR NOT err MATCHES "needs -cov")
  message(FATAL_ERROR "-uncovered without -cov: ${rc}\n${out}${err}")
endif()
file(WRITE ${OUT}/notcov.json "{}")
execute_process(COMMAND ${MDEBUG} ${SV} -top tb -cov ${OUT}/notcov.json RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 2 OR NOT err MATCHES "is not a coverage database")
  message(FATAL_ERROR "-cov of a file that is not a database: ${rc}\n${out}${err}")
endif()
