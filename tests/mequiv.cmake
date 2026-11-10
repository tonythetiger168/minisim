# mequiv self-test: pairs of designs under tests/equiv that must be proved
# equivalent, pairs that must not be (with the failing point named), and the
# same answers on one thread as on many.
#   cmake -DMEQUIV=<mequiv> -DSRC=<source dir> -P mequiv.cmake
set(D ${SRC}/tests/equiv)

# check(<expected exit> <text the output must contain, or ""> <mequiv args...>)
function(check want text)
  execute_process(COMMAND ${MEQUIV} ${ARGN} WORKING_DIRECTORY ${D}
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT rc EQUAL ${want})
    message(FATAL_ERROR "mequiv ${ARGN}: exit ${rc}, expected ${want}\n${out}${err}")
  endif()
  if(NOT "${text}" STREQUAL "")
    string(FIND "${out}${err}" "${text}" pos)
    if(pos EQUAL -1)
      message(FATAL_ERROR "mequiv ${ARGN}: missing '${text}'\n${out}${err}")
    endif()
  endif()
  set(out "${out}" PARENT_SCOPE)
endfunction()

# RTL against a hand-made netlist of library cells; flops matched y -> y_reg_N_.
check(0 "passed 13, failed 0" -ref alu_rtl.sv -impl alu_gates.v cells.v)
check(0 "13 (8 settled structurally" -ref alu_rtl.sv -impl alu_gates.v cells.v -threads 1)
# One wrong wire: the point, and an input pattern that shows it.
check(1 "FAIL  next-state  y[2]" -ref alu_rtl.sv -impl alu_gates_bug.v cells.v)
check(1 "reference = 0, implementation = 1" -ref alu_rtl.sv -impl alu_gates_bug.v cells.v -threads 1)

# Netlists from Yosys: a state machine, a datapath (generate, functions, casez,
# signed divide, a negative-edge flop) and a memory with a latch and an
# asynchronously set counter.
check(0 "passed 34, failed 0" -ref fsm.sv -impl fsm_net.v)
check(0 "failed 0, aborted 0" -ref datapath.sv -impl datapath_net.v)
check(0 "failed 0, aborted 0" -ref mem.sv -impl mem_net.v)
# Netlist against netlist, and a design against itself (all structural).
check(0 "34 (34 settled structurally" -ref fsm_net.v -impl fsm_net.v)

# Flops with meaningless instance names, storing the value inverted.
check(0 "3 state elements (3 stored inverted)" -ref invreg.sv -impl invreg_gates.v cells.v)

# Latches, an 'x the netlist fills in, and a black box on both sides.
check(0 "1 flops, 4 latches" -top misc -blackbox ram -ref misc.sv -impl misc_gates.v cells.v)
check(0 "passed 27" -top misc -blackbox ram -match toggle=tff.Q -ref misc.sv -impl misc_gates.v cells.v)
# +define+BUG reaches only the implementation; the don't-care does not hide it.
check(1 "FAIL  output      y[" -top misc -blackbox ram -ref misc.sv -impl misc_gates.v cells.v +define+BUG)
# Swapped: the netlist as reference has no don't-cares, so the RTL's X fails.
check(1 "implementation = X" -top misc -blackbox ram -ref misc_gates.v cells.v -impl misc.sv)

# Bit-level and block-level feedback that is not a loop; and one that is.
check(0 "passed 7, failed 0" -ref bus.sv -impl bus_add.sv)
check(2 "combinational loop through" -ref loop.sv -impl loop.sv)

# A report file, and the same verdict on one thread.
check(1 "" -ref alu_rtl.sv -impl alu_gates_bug.v cells.v -report ${CMAKE_CURRENT_BINARY_DIR}/mequiv_report.json)
file(READ ${CMAKE_CURRENT_BINARY_DIR}/mequiv_report.json rep)
if(NOT rep MATCHES "\"name\": \"y\\[2\\]\", \"status\": \"fail\"")
  message(FATAL_ERROR "report does not show the failing point:\n${rep}")
endif()

# The same netlists with the cells read from a Liberty library instead of
# Verilog models: functions, ff groups (clear, preset) and a latch group.
check(0 "passed 13, failed 0" -ref alu_rtl.sv -impl alu_gates.v -lib cells.lib)
check(1 "FAIL  next-state  y[2]" -ref alu_rtl.sv -impl alu_gates_bug.v -lib cells.lib)
check(0 "(3 stored inverted)" -ref invreg.sv -impl invreg_gates.v -lib cells.lib)
check(0 "1 flops, 4 latches" -top misc -blackbox ram -ref misc.sv -impl misc_gates.v -lib cells.lib)
# Verilog models against Liberty for the same netlist: the two views agree.
check(0 "passed 13" -lib cells.lib -ref alu_gates.v cells.v -impl alu_gates.v)
check(2 "unknown module" -ref alu_rtl.sv -impl alu_gates.v)
