# Assertions and power in the waveform (statview.inc): $fsdbDumpSVA gives
# each assertion and cover .passes / .fails counters that step when an
# attempt ends; -vcd-power gives each power domain's simstate and each
# supply net's state and voltage, with the times msim -upf_verbose reports.
#   cmake -DMSIM=<msim> -DCHK=<test_fst> -DSRC=<source dir> -DOUT=<dir> -P msim_wave_state.cmake
file(REMOVE_RECURSE ${OUT})
file(MAKE_DIRECTORY ${OUT})
function(wave name file var)
  execute_process(COMMAND ${CHK} ${OUT}/${file} RESULT_VARIABLE rc OUTPUT_VARIABLE o ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${name}: test_fst ${file} failed: ${err}")
  endif()
  set(${var} "\n${o}" PARENT_SCOPE)
endfunction()
function(has name text)
  foreach(want ${ARGN})
    if(NOT text MATCHES "${want}")
      message(FATAL_ERROR "${name}: expected '${want}' in\n${text}")
    endif()
  endforeach()
endfunction()

execute_process(COMMAND ${MSIM} ${SRC}/tests/debug/sva_wave.sv WORKING_DIRECTORY ${OUT}
                RESULT_VARIABLE rc OUTPUT_VARIABLE o ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT o MATCHES "45 a_gnt failed\n85 a_gnt failed")
  message(FATAL_ERROR "sva: msim exited ${rc}\n${err}\n${o}")
endif()
wave(sva sva.fst w)
has(sva "${w}" "\n0 tb.u.a_gnt.fails 0+\n" "\n45 tb.u.a_gnt.fails 0+1\n" "\n85 tb.u.a_gnt.fails 0+10\n"
    "\n15 tb.u.c_req.passes 0+1\n")
string(REGEX MATCHALL "tb.u.a_gnt.fails" n "${w}")
list(LENGTH n n)
if(NOT n EQUAL 3)
  message(FATAL_ERROR "sva: a_gnt.fails should change twice after 0\n${w}")
endif()

set(U ${SRC}/tests/upf)
execute_process(COMMAND ${MSIM} ${U}/gating.sv -upf ${U}/gating.upf -fst power.fst -vcd-power WORKING_DIRECTORY ${OUT}
                RESULT_VARIABLE rc OUTPUT_QUIET ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "power: msim exited ${rc}\n${err}")
endif()
wave(power power.fst w)
has(power "${w}" "\n0 tb.upf.PD_CORE.simstate 101\n" "\n2 tb.upf.PD_CORE.simstate 000\n" "\n45 tb.upf.PD_CORE.simstate 101\n"
    "\n66 tb.upf.PD_CORE.simstate 000\n" "\n1 tb.upf.VDD.mV 0+1111101000\n" "\n45 tb.upf.VDD_CORE.state 00\n"
    "\n66 tb.upf.VDD_CORE.state 01\n")

# The same as VCD, and the option names the FST spelling too.
execute_process(COMMAND ${MSIM} ${U}/gating.sv -upf ${U}/gating.upf -vcd power.vcd -fst-power WORKING_DIRECTORY ${OUT}
                RESULT_VARIABLE rc OUTPUT_QUIET)
file(READ ${OUT}/power.vcd v)
has(power-vcd "${v}" "\\$scope module upf \\$end" "\\$scope module PD_CORE \\$end" " simstate \\$end")
