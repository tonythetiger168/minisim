# saif::load sums a large waveform a group of signals at a time, so that only
# one group's value changes are in memory. The SAIF must be the same as from
# one pass: MINISIM_SAIF_GROUPS forces the grouped path on a small file.
#   cmake -DMSIM=<msim> -DSRC=<source dir> -DFORMATS=vcd,fst -P saif_group.cmake
set(D ${SRC}/tests/saif)
set(B ${CMAKE_CURRENT_BINARY_DIR}/saif_group)
file(MAKE_DIRECTORY ${B})
string(REPLACE "," ";" FORMATS "${FORMATS}")
foreach(fmt ${FORMATS})
  foreach(g 1 2 5)
    set(ENV{MINISIM_SAIF_GROUPS} ${g})
    execute_process(COMMAND ${MSIM} ${D}/cells.sv -top top -${fmt} ${B}/w${g}.${fmt} -saif ${B}/${fmt}${g}.saif
                    WORKING_DIRECTORY ${B} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT rc EQUAL 0)
      message(FATAL_ERROR "msim -${fmt} -saif (groups ${g}) exited ${rc}\n${out}${err}")
    endif()
    file(READ ${B}/${fmt}${g}.saif s${g})
  endforeach()
  unset(ENV{MINISIM_SAIF_GROUPS})
  string(FIND "${s1}" "(TC 1)" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "${fmt}: one pass has no toggles\n${s1}")
  endif()
  foreach(g 2 5)
    if(NOT s${g} STREQUAL s1)
      message(FATAL_ERROR "${fmt}: SAIF from ${g} groups differs from one pass\n--- 1\n${s1}--- ${g}\n${s${g}}")
    endif()
  endforeach()
endforeach()
# A name that is not a SAIF identifier is escaped (a leading digit included),
# so OpenSTA's read_saif takes it: an escaped Verilog name \1'b0 is the tie
# net of many routed netlists.
file(WRITE ${B}/esc.sv "module top; wire \\1'b0 ; reg c = 0; assign \\1'b0 = 1'b0; initial #10 c = 1; endmodule\n")
execute_process(COMMAND ${MSIM} ${B}/esc.sv -top top -vcd ${B}/esc.vcd -saif ${B}/esc.saif
                WORKING_DIRECTORY ${B} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "msim esc.sv -saif exited ${rc}\n${out}${err}")
endif()
file(READ ${B}/esc.saif s)
string(FIND "${s}" "(\\1\\'b0 (T0 10)" pos)
if(pos EQUAL -1)
  message(FATAL_ERROR "the name 1'b0 is not escaped as \\1\\'b0\n${s}")
endif()
