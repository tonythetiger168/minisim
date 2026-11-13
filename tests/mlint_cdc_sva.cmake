# mlint -cdc-sva: the assertions it writes compile with the design under msim,
# and on tests/lint/cdc_sva.sv only the crossing that is too fast fails.
#   cmake -DMLINT=<mlint> -DMSIM=<msim> -DSRC=<source dir> -DOUT=<dir> -P mlint_cdc_sva.cmake
file(MAKE_DIRECTORY ${OUT})
set(sva ${OUT}/cdc_sva_props.sv)
execute_process(COMMAND ${MLINT} ${SRC}/tests/lint/cdc_sva.sv -top cdc_sva_tb -only cdc -cdc-sva ${sva}
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT err MATCHES "9 assertions")
  message(FATAL_ERROR "mlint -cdc-sva exited ${rc}:\n${out}${err}")
endif()
file(READ ${sva} text)
foreach(want
    "module mlint_cdc_sva;"
    "cdc_onebit_cdc_sva_tb_dut_gray: assert property (@(posedge cdc_sva_tb.fclk) disable iff (!cdc_sva_tb.rst_n) $countones(cdc_sva_tb.dut.gray ^ $past(cdc_sva_tb.dut.gray)) <= 1)"
    "always @(cdc_sva_tb.dut.tog) if (cdc_sva_tb.rst_n) begin"
    "cdc_stable_cdc_sva_tb_dut_req_to_cdc_sva_tb_sclk: assert"
    "always @(cdc_sva_tb.dut.req) if (cdc_sva_tb.rst_n) begin"
    "cdc_handshake_cdc_sva_tb_dut_hreq: assert (seen_handshake_cdc_sva_tb_dut_hreq || first_handshake_cdc_sva_tb_dut_hreq)")
  string(FIND "${text}" "${want}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "-cdc-sva output missing: ${want}\n${text}")
  endif()
endforeach()
execute_process(COMMAND ${MSIM} ${SRC}/tests/lint/cdc_sva.sv ${sva} -top cdc_sva_tb -top mlint_cdc_sva
                WORKING_DIRECTORY ${OUT} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
set(all "${out}${err}")
if(NOT all MATCHES "cdc_sva_tb.dut.tog changed again before two cdc_sva_tb.sclk edges")
  message(FATAL_ERROR "the fast toggle did not fail its assertion (msim exited ${rc}):\n${all}")
endif()
if(NOT all MATCHES "cdc_sva_tb.dut.breq changed again before cdc_sva_tb.dut.ba2 answered its last change")
  message(FATAL_ERROR "the impatient handshake did not fail its assertion:\n${all}")
endif()
if(all MATCHES "dut.gray changed|dut.req changed|dut.hreq changed|dut.hack changed")
  message(FATAL_ERROR "a safe crossing failed its assertion:\n${all}")
endif()
# A storage wider than what the reader takes (a FIFO memory) gets no held
# check; the plain qualified bus beside it does.
set(mem ${OUT}/cdc_mem_props.sv)
execute_process(COMMAND ${MLINT} ${SRC}/tests/lint/cdc_mem.sv -top cdc_mem -only cdc -cdc-sva ${mem}
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
file(READ ${mem} text)
if(NOT rc EQUAL 0 OR NOT text MATCHES "cdc_held_cdc_mem_bus_at_cdc_mem_v2" OR text MATCHES "cdc_held_cdc_mem_mem")
  message(FATAL_ERROR "-cdc-sva on cdc_mem.sv:\n${err}${text}")
endif()
