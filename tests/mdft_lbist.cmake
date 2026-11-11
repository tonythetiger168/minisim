# mdft logic BIST self-test.
#   cmake -DMDFT=<mdft> -DMSIM=<msim> -DSRC=<source dir> -DOUT=<scratch dir> -P mdft_lbist.cmake
# The wrapper is signed by simulation, must pass its own testbench, must fail
# on a design with a changed gate, and an X source must be reported.
set(T ${SRC}/tests/dft/lbist)
file(REMOVE_RECURSE ${OUT})
file(MAKE_DIRECTORY ${OUT}/x)

execute_process(COMMAND ${MDFT} -lbist ${T}/sdut.sv -top sdut -scandef ${T}/chains.json -lbist_patterns 200
                        -lbist_sign -lbist_msim ${MSIM} -lbist_dump loads.txt
                WORKING_DIRECTORY ${OUT} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT out MATCHES "2 chains, longest 5, 200 patterns, 1206 clock cycles" OR
   NOT out MATCHES "golden signature ([0-9a-f]+)")
  message(FATAL_ERROR "mdft -lbist: rc=${rc}\n${out}${err}")
endif()
set(sig ${CMAKE_MATCH_1})
file(READ ${OUT}/sdut_lbist.sv rtl)
if(NOT rtl MATCHES "GOLDEN = 32'h${sig}")
  message(FATAL_ERROR "the golden signature ${sig} is not in sdut_lbist.sv")
endif()
# The pattern dump: 200 patterns, two chains, 4 and 5 bits.
file(STRINGS ${OUT}/loads.txt loads)
list(LENGTH loads n)
list(GET loads 0 l0)
list(GET loads 1 l1)
if(NOT n EQUAL 400 OR NOT l0 MATCHES "^0 chain0 [01][01][01][01]$" OR NOT l1 MATCHES "^0 chain1 [01][01][01][01][01]$")
  message(FATAL_ERROR "loads.txt: ${n} lines, '${l0}', '${l1}'")
endif()

function(simulate want)
  execute_process(COMMAND ${MSIM} ${ARGN} WORKING_DIRECTORY ${OUT} OUTPUT_VARIABLE out ERROR_VARIABLE err)
  string(FIND "${out}" "${want}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "msim ${ARGN}: expected '${want}'\n${out}${err}")
  endif()
endfunction()
simulate("sdut_lbist_tb: PASS" ${T}/sdut.sv sdut_lbist.sv sdut_lbist_tb.sv -top sdut_lbist_tb)
# One changed gate in the design: the signature differs.
file(READ ${T}/sdut.sv src)
string(REPLACE "r[7:4] + r[3:0] + a," "r[7:4] + r[3:0] + a + 4'd1," faulty "${src}")
if(faulty STREQUAL src)
  message(FATAL_ERROR "sdut.sv changed: update the fault injection in this test")
endif()
file(WRITE ${OUT}/faulty.sv "${faulty}")
simulate("sdut_lbist_tb: FAIL" faulty.sv sdut_lbist.sv sdut_lbist_tb.sv -top sdut_lbist_tb)
# An unscanned, uninitialized flop that reaches the chains.
string(REPLACE "wire [8:0] d = " "logic u; always_ff @(posedge clk) if (!scan_en) u <= u ^ a[0];\n  wire [8:0] d = {8'b0, u} ^ " xsrc "${src}")
file(WRITE ${OUT}/xsrc.sv "${xsrc}")
execute_process(COMMAND ${MDFT} -lbist xsrc.sv -top sdut -scandef ${T}/chains.json -lbist_patterns 20 -lbist_out x
                        -lbist_sign -lbist_msim ${MSIM}
                WORKING_DIRECTORY ${OUT} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 1 OR NOT err MATCHES "the signature is unknown")
  message(FATAL_ERROR "X source: rc=${rc}\n${out}${err}")
endif()
# The IJTAG access that came with the wrapper retargets and simulates.
execute_process(COMMAND ${MDFT} -icl sdut_lbist.icl -pdl sdut_lbist.pdl -iproc bist.run -icl_rtl acc.sv -tb acc_tb.sv
                WORKING_DIRECTORY ${OUT} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT out MATCHES "3 iApply, 3 scans, 105 scan bits")
  message(FATAL_ERROR "LBIST ICL/PDL: rc=${rc}\n${out}${err}")
endif()
simulate("acc_tb: PASS" acc.sv acc_tb.sv -top acc_tb)
# Different widths and seed give a different, still deterministic, plan.
execute_process(COMMAND ${MDFT} -lbist ${T}/sdut.sv -top sdut -scandef ${T}/chains.json -lbist_patterns 50 -lbist_prpg 17
                        -lbist_misr 24 -lbist_seed 1ACE -lbist_out x -lbist_sign -lbist_msim ${MSIM}
                WORKING_DIRECTORY ${OUT} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT out MATCHES "PRPG 17 bits, MISR 24 bits")
  message(FATAL_ERROR "mdft -lbist widths: rc=${rc}\n${out}${err}")
endif()
simulate("sdut_lbist_tb: PASS" ${T}/sdut.sv x/sdut_lbist.sv x/sdut_lbist_tb.sv -top sdut_lbist_tb)
message(STATUS "mdft_lbist: ok")
