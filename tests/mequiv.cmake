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
