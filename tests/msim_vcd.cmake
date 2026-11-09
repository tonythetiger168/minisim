# msim -vcd <file> writes the named waveform with the child instance's signals;
# without -vcd no dump.vcd appears.
set(dir ${CMAKE_CURRENT_BINARY_DIR}/msim_vcd_test)
file(REMOVE_RECURSE ${dir})
file(MAKE_DIRECTORY ${dir})
execute_process(COMMAND ${MSIM} ${SRC}/examples/cli/adder.sv ${SRC}/examples/cli/adder_tb.sv
                        -top adder_tb -vcd waves.vcd
                WORKING_DIRECTORY ${dir} RESULT_VARIABLE rc OUTPUT_QUIET)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "msim -vcd exited ${rc}")
endif()
if(NOT EXISTS ${dir}/waves.vcd)
  message(FATAL_ERROR "waves.vcd not written")
endif()
file(READ ${dir}/waves.vcd vcd)
foreach(want "$enddefinitions" "$dumpvars" "$scope module adder_tb $end" "$scope module u $end" " sum $end" " clk $end")
  string(FIND "${vcd}" "${want}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "waves.vcd lacks '${want}'")
  endif()
endforeach()
if(EXISTS ${dir}/dump.vcd)
  message(FATAL_ERROR "dump.vcd written without +vcd")
endif()

file(REMOVE ${dir}/waves.vcd)
execute_process(COMMAND ${MSIM} ${SRC}/examples/cli/spin.sv -maxtime 10
                WORKING_DIRECTORY ${dir} RESULT_VARIABLE rc OUTPUT_QUIET)
if(EXISTS ${dir}/dump.vcd)
  message(FATAL_ERROR "dump.vcd written with no -vcd option")
endif()
