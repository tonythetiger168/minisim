# mdft boundary scan (IEEE 1149.1) and IJTAG (IEEE 1687) self-test.
#   cmake -DMDFT=<mdft> -DMSIM=<msim> -DSRC=<source dir> -DOUT=<scratch dir> -P mdft_jtag.cmake
# Each flow writes RTL and a self-checking testbench, and msim runs them: the
# testbench must print PASS, and must print FAIL once the RTL is broken.
set(T ${SRC}/tests/dft/jtag)
file(REMOVE_RECURSE ${OUT})
file(MAKE_DIRECTORY ${OUT})

function(run_ok)
  execute_process(COMMAND ${ARGV} WORKING_DIRECTORY ${OUT}
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  set(out "${out}${err}" PARENT_SCOPE)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${ARGV} exited ${rc}\n${out}${err}")
  endif()
endfunction()
function(want text)
  string(FIND "${out}" "${text}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing: ${text}\n${out}")
  endif()
endfunction()
function(simulate top want_line)
  execute_process(COMMAND ${MSIM} ${ARGN} -top ${top} WORKING_DIRECTORY ${OUT}
                  OUTPUT_VARIABLE out ERROR_VARIABLE err)
  string(FIND "${out}" "${want_line}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "msim ${top}: expected '${want_line}'\n${out}${err}")
  endif()
endfunction()

# --- boundary scan -----------------------------------------------------------
run_ok(${MDFT} -bscan ${T}/chip.sv -top chip -bscan_idcode 149511 -bscan_tristate d=d_o,d_oe,d_i
       -bscan_highz -svf chip.svf -tb chip_tb.sv)
want("boundary scan for chip: 19 cells, 4-bit instruction register")
file(READ ${OUT}/chip_bscan.bsdl bsdl)
foreach(line
    "attribute INSTRUCTION_CAPTURE of chip_bscan : entity is \"0001\";"
    "\"0000\" & \"0000000101001001\" & \"01010001000\" & \"1\";"
    "\"BYPASS (1111)\";"
    "\"18 (BC_4, clk, clock, X)\";"
    "\"5 (BC_1, *, control, 0),\""
    "\"4 (BC_1, d(1), output3, X, 5, 0, Z),\""
    "\"6 (BC_1, q, output3, X, 11, 1, Z),\""
    "\"0 (BC_4, aux, observe_only, X),\"")
  string(FIND "${bsdl}" "${line}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "chip_bscan.bsdl lacks: ${line}\n${bsdl}")
  endif()
endforeach()
file(READ ${OUT}/chip.svf svf)
if(NOT svf MATCHES "SIR 4 TDI \\(F\\) TDO \\(1\\) MASK \\(F\\);" OR
   NOT svf MATCHES "SDR 32 TDI \\(00000000\\) TDO \\(00149511\\) MASK \\(FFFFFFFF\\);")
  message(FATAL_ERROR "chip.svf: no instruction capture or IDCODE check\n${svf}")
endif()
simulate(chip_tb "chip_tb: PASS" ${T}/chip.sv chip_bscan.sv chip_tb.sv)
# The generated BSDL passes the checker.
run_ok(${MDFT} -bsdl chip_bscan.bsdl -Werror)
want("19 boundary cells, 0 findings")
# A wrapper that does not drive the pins in EXTEST fails its own test.
file(READ ${OUT}/chip_bscan.sv rtl)
string(REPLACE "wire mode_out = extest | clamp;" "wire mode_out = clamp;" broken "${rtl}")
file(WRITE ${OUT}/chip_broken.sv "${broken}")
simulate(chip_tb "chip_tb: FAIL" ${T}/chip.sv chip_broken.sv chip_tb.sv)
# ... and so does one whose boundary register is a bit short of the BSDL.
string(REPLACE "bsr <= {tdi, bsr[18:1]};" "bsr <= {tdi, tdi, bsr[17:1]};" broken "${rtl}")
file(WRITE ${OUT}/chip_broken.sv "${broken}")
simulate(chip_tb "chip_tb: FAIL" ${T}/chip.sv chip_broken.sv chip_tb.sv)
# Plain outputs, INTEST, a user instruction, a 5-bit instruction register.
run_ok(${MDFT} -bscan ${T}/chip.sv -top chip -bscan_out . -bscan_exclude aux -bscan_intest -bscan_user ijtag
       -bscan_ir 5 -bscan_no_idcode -tb plain_tb.sv -quiet)
simulate(plain_tb "plain_tb: PASS" ${T}/chip.sv chip_bscan.sv plain_tb.sv)

# --- the BSDL checker ---------------------------------------------------------
run_ok(${MDFT} -bsdl ${T}/bad.bsdl)
foreach(msg
    "[BSD007] the TAP port 'tdo' (TAP_SCAN_OUT) must be a single 'out bit'"
    "[BSD002] INSTRUCTION_CAPTURE must end in 01, found '010'"
    "[BSD002] the opcode 001 belongs to both SAMPLE and IDCODE"
    "[BSD002] USER: the opcode '10' is not 3 bits"
    "[BSD002] the mandatory instruction PRELOAD is missing"
    "[BSD002] BYPASS must include the all-ones opcode"
    "[BSD003] bit 0 of IDCODE_REGISTER must be 1"
    "[BSD004] cell 6 is not described"
    "[BSD004] cell 7 is outside BOUNDARY_LENGTH 7"
    "[BSD005] cell 1: a(2) is outside the port's range"
    "[BSD005] cell 2: an output2 cell on the input port 'a'"
    "[BSD005] cell 5: the linkage port 'vdd' cannot have a cell"
    "[BSD005] cell 7: 'ghost' is not a port"
    "[BSD006] cell 3: an output3 cell needs a control cell"
    "[BSD006] cell 4: cell 0 is not a control cell"
    "[BSD008] the port 'nc' has no boundary cell"
    "[BSD009] the port 'a' has 2 bits but 3 pins"
    "[BSD009] the port 'tdo' is not in the pin map")
  want("${msg}")
endforeach()
run_ok(${MDFT} -bsdl ${T}/bad2.bsdl)
want("[BSD001] the attribute INSTRUCTION_LENGTH is missing")
want("[BSD007] no port carries the attribute TAP_SCAN_IN")
execute_process(COMMAND ${MDFT} -bsdl ${T}/bad.bsdl -Werror -tb x.sv WORKING_DIRECTORY ${OUT}
                RESULT_VARIABLE rc OUTPUT_QUIET ERROR_QUIET)
if(rc EQUAL 0 OR EXISTS ${OUT}/x.sv)
  message(FATAL_ERROR "a BSDL file with errors must not produce tests (rc=${rc})")
endif()

# --- IJTAG ---------------------------------------------------------------------
# A bare 1687 scan interface, then the same network behind a TAP.
run_ok(${MDFT} -icl ${T}/net.icl -pdl ${T}/net.pdl -iproc all -icl_rtl net.sv -svf net.svf -tb net_tb.sv)
want("IJTAG network Top: 5 scan registers (19 bits), 3 scan muxes")
want("8 iApply, 13 scans, 145 scan bits")
simulate(net_tb "net_tb: PASS" net.sv net_tb.sv)
file(READ ${OUT}/net.svf svf)
# The first iApply opens sib1 (2 bits on the path), then writes the 8-bit TDR.
if(NOT svf MATCHES "SDR 2 TDI \\(1\\);\nSDR 10 TDI \\(00B\\);" OR
   NOT svf MATCHES "SDR 10 TDI \\(00B\\) TDO \\(1B0\\) MASK \\(1F8\\);")
  message(FATAL_ERROR "net.svf: unexpected retargeting\n${svf}")
endif()
file(READ ${OUT}/net.sv rtl)
string(REPLACE "8'b00111100" "8'b00111101" broken "${rtl}")
if(broken STREQUAL rtl)
  message(FATAL_ERROR "net.sv: the reset value of sub.cfg is not where the test expects it")
endif()
file(WRITE ${OUT}/net_broken.sv "${broken}")
simulate(net_tb "net_tb: FAIL" net_broken.sv net_tb.sv)
run_ok(${MDFT} -icl ${T}/net.icl -pdl ${T}/net.pdl -iproc all -icl_rtl tap.sv -svf tap.svf -tb tap_tb.sv -ijtag_tap)
simulate(tap_tb "tap_tb: PASS" tap.sv tap_tb.sv)
file(READ ${OUT}/tap.svf svf)
if(NOT svf MATCHES "SIR 4 TDI \\(2\\);\nSDR 2 TDI \\(1\\);")
  message(FATAL_ERROR "tap.svf: the IJTAG instruction is not loaded first\n${svf}")
endif()
# Without PDL: the network and its reset path.
run_ok(${MDFT} -icl ${T}/net.icl)
want("scan path after reset: 2 bits")
# PDL errors name the file and line.
file(WRITE ${OUT}/err.pdl "iWrite tdr1.R 0x1FF\n")
execute_process(COMMAND ${MDFT} -icl ${T}/net.icl -pdl err.pdl WORKING_DIRECTORY ${OUT}
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 2 OR NOT err MATCHES "err.pdl:1: the value 0x1FF does not fit in 8 bits")
  message(FATAL_ERROR "PDL error: rc=${rc}\n${out}${err}")
endif()
file(WRITE ${OUT}/err.pdl "\niRead sens.temp 1\niApply\niWrite nothere 1\n")
execute_process(COMMAND ${MDFT} -icl ${T}/net.icl -pdl err.pdl WORKING_DIRECTORY ${OUT}
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 2 OR NOT err MATCHES "err.pdl:4: .*'nothere' is not a data signal in Top")
  message(FATAL_ERROR "PDL error: rc=${rc}\n${out}${err}")
endif()
message(STATUS "mdft_jtag: ok")
