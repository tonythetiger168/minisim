# Virtual prototype self-test: the design reaches the C models through
# mvp_axil_slave (memory, byte strobes, a decode error, the UART's character
# time and its interrupt), and -mvp finds the runtime.
#   cmake -DMSIM=<msim> -DSRC=<source dir> -DOUT=<dir> -P mvp.cmake
file(REMOVE_RECURSE ${OUT})
file(MAKE_DIRECTORY ${OUT})
set(mvp ${SRC}/mvp)
set(models ${mvp}/models/ram.c ${mvp}/models/uart.c)
set(env MVP_RAM_SIZE=0x1000 MVP_UART_BASE=0x1000)

function(run what)
  execute_process(COMMAND ${CMAKE_COMMAND} -E env ${env} ${MSIM} ${ARGN}
                  WORKING_DIRECTORY ${OUT} RESULT_VARIABLE rc
                  OUTPUT_VARIABLE out ERROR_VARIABLE err)
  set(out "${out}" PARENT_SCOPE)
  set(err "${err}" PARENT_SCOPE)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${what}: msim exited ${rc}\n${out}${err}")
  endif()
  if(NOT out MATCHES "VPROTO PASSED")
    message(FATAL_ERROR "${what}: the testbench reported a failure\n${out}${err}")
  endif()
endfunction()

# The files named by hand.
run("explicit files" ${mvp}/sv/mvp.sv ${SRC}/tests/vproto/soc_tb.sv ${mvp}/mvp.c ${models}
    -top soc_tb)
# The memory map is printed once, and the models' own output arrives in order.
foreach(want "0x00000000..0x00000fff  ram" "0x00001000..0x0000100f  uart")
  string(FIND "${out}" "${want}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "the memory map lacks '${want}':\n${out}")
  endif()
endforeach()
# The UART printed "Hi" through the model, not through $display.
if(NOT out MATCHES "H")
  message(FATAL_ERROR "the UART model printed nothing:\n${out}")
endif()
# One character at 115200 baud is 86.8 us; the interrupt waited for it.
if(NOT out MATCHES "tx took 8[0-9]+")
  message(FATAL_ERROR "the character time was not respected:\n${out}")
endif()

# -mvp adds mvp.sv and the runtime, so only the models are left to name.
run("-mvp" -mvp ${SRC}/tests/vproto/soc_tb.sv ${models} -top soc_tb)

# An overlapping device is refused rather than silently answering both.
execute_process(COMMAND ${CMAKE_COMMAND} -E env MVP_RAM_SIZE=0x10000 MVP_UART_BASE=0x1000
                        ${MSIM} -mvp ${SRC}/tests/vproto/soc_tb.sv ${models} -top soc_tb
                WORKING_DIRECTORY ${OUT} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT err MATCHES "overlaps")
  message(FATAL_ERROR "an overlapping device was accepted: exit ${rc}\n${out}${err}")
endif()

message(STATUS "mvp: ok")
