# mdft compressed-pattern responses and compactor X masking self-test.
#   cmake -DMDFT=<mdft> -DMATPG=<matpg> -DMSIM=<msim> -DSRC=<source dir> -DOUT=<scratch dir> -P mdft_edt_resp.cmake
set(T ${SRC}/tests/atpg)
set(SD ${SRC}/tests/dft/lbist/scan_chains.json)
file(REMOVE_RECURSE ${OUT})
file(MAKE_DIRECTORY ${OUT} ${OUT}/plain ${OUT}/masked)
function(run_ok)
  execute_process(COMMAND ${ARGV} WORKING_DIRECTORY ${OUT} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  set(out "${out}${err}" PARENT_SCOPE)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${ARGV} exited ${rc}\n${out}${err}")
  endif()
endfunction()
function(simulate want)
  execute_process(COMMAND ${MSIM} ${ARGN} WORKING_DIRECTORY ${OUT} OUTPUT_VARIABLE out ERROR_VARIABLE err)
  string(FIND "${out}" "${want}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "msim ${ARGN}: expected '${want}'\n${out}${err}")
  endif()
endfunction()

# 1. No unknowns: every captured cell is known and shows at the channel.
run_ok(${MATPG} ${T}/scan.v -cubes cubes.txt -quiet)
run_ok(${MDFT} -edt_encode -scandef ${SD} -edt_cubes cubes.txt -edt_patterns p1.txt -edt_ring 8 -threads 1 ${T}/scan.v)
if(NOT out MATCHES "23 of 23 cubes encoded" OR NOT out MATCHES "responses: 0 of 184 captured cells are unknown; the compactor shows 100.0%")
  message(FATAL_ERROR "mdft -edt_encode with a netlist:\n${out}")
endif()
run_ok(${MDFT} -edt_encode -scandef ${SD} -edt_cubes cubes.txt -edt_patterns p4.txt -edt_ring 8 -threads 4 ${T}/scan.v)
file(READ ${OUT}/p1.txt p1)
file(READ ${OUT}/p4.txt p4)
if(NOT p1 STREQUAL p4 OR NOT p1 MATCHES "\n0 capture chain0 [01][01][01][01]\n" OR NOT p1 MATCHES "\n22 out 0 [01]+\n" OR p1 MATCHES " mask ")
  message(FATAL_ERROR "the responses depend on the thread count, or lines are missing:\n${p1}")
endif()

# 2. Three cells of chain1 capture an unknown (a flop outside the chains).
file(READ ${T}/scan.v net)
string(REPLACE "SDFF R1 (.D(n30)," "SDFF R1 (.D(xd1)," net "${net}")
string(REPLACE "SDFF R5 (.D(n54)," "SDFF R5 (.D(xd5)," net "${net}")
string(REPLACE "SDFF R7 (.D(n9)," "SDFF R7 (.D(xd7)," net "${net}")
string(REPLACE "  BUF SO0 " "  wire xq, xd1, xd5, xd7;
  DFF XR (.D(xq), .CK(clk), .Q(xq));
  XOR2 XU1 (.A(n30), .B(xq), .Y(xd1));
  XOR2 XU5 (.A(n54), .B(xq), .Y(xd5));
  XOR2 XU7 (.A(n9), .B(xq), .Y(xd7));
  BUF SO0 " net "${net}")
if(NOT net MATCHES "xd7")
  message(FATAL_ERROR "tests/atpg/scan.v changed: the X source was not added")
endif()
file(WRITE ${OUT}/scanx.v "${net}")
run_ok(${MATPG} scanx.v -cubes cubesx.txt -quiet)
# Without masking the unknowns hide most of chain0 too.
run_ok(${MDFT} -edt scanx.v ${T}/generic_cells.v -top scan -scandef ${SD} -edt_cubes cubesx.txt -edt_patterns px.txt -edt_ring 8
       -edt_netlist scanx.v -edt_out plain)
if(NOT out MATCHES "responses: ([0-9]+) of [0-9]+ captured cells are unknown; the compactor shows ([0-9.]+)% of the known ones\n")
  message(FATAL_ERROR "mdft -edt -edt_netlist:\n${out}")
endif()
set(plain ${CMAKE_MATCH_2})
if(CMAKE_MATCH_1 EQUAL 0 OR NOT plain LESS 60)
  message(FATAL_ERROR "expected unknown cells to hide known ones:\n${out}")
endif()
file(READ ${OUT}/px.txt px)
if(NOT px MATCHES " capture chain1 [01x]*x" OR NOT px MATCHES " out 0 [01]*x")
  message(FATAL_ERROR "px.txt shows no unknown:\n${px}")
endif()
# With masking chain0 is observed alone and every known cell of it is seen.
run_ok(${MDFT} -edt scanx.v ${T}/generic_cells.v -top scan -scandef ${SD} -edt_cubes cubesx.txt -edt_patterns pm.txt -edt_ring 8
       -edt_xmask -edt_netlist scanx.v -edt_out masked)
if(NOT out MATCHES "the compactor shows ([0-9.]+)% of the known ones \\(([0-9.]+)% without masking; ([0-9]+) channel masks used\\)")
  message(FATAL_ERROR "mdft -edt -edt_xmask:\n${out}")
endif()
if(NOT CMAKE_MATCH_1 GREATER plain OR NOT CMAKE_MATCH_2 STREQUAL plain OR CMAKE_MATCH_3 EQUAL 0)
  message(FATAL_ERROR "masking did not help: ${out}")
endif()
file(READ ${OUT}/pm.txt pm)
if(NOT pm MATCHES "\n[0-9]+ mask 10\n")
  message(FATAL_ERROR "pm.txt has no mask that selects chain0:\n${pm}")
endif()
file(READ ${OUT}/masked/scan_edt.sv rtl)
if(NOT rtl MATCHES "edt_mask_in" OR NOT rtl MATCHES "mask <= mask_sr")
  message(FATAL_ERROR "the masked compactor is missing")
endif()

# 3. In simulation: the wrapper in shift mode with random masks, then the
# patterns with capture, on the good netlist and on one with a defect.
if(MSIM)
  foreach(d plain masked)
    simulate("scan_edt_tb: PASS" ${d}/scan_edt_tb.sv ${d}/scan_edt.sv scanx.v ${T}/generic_cells.v)
    simulate("scan_edt_pat_tb: PASS" ${d}/scan_edt_pat_tb.sv ${d}/scan_edt.sv scanx.v ${T}/generic_cells.v)
  endforeach()
  string(REPLACE "INV U12 (.A(q3), .Y(n12));" "BUF U12 (.A(q3), .Y(n12));" bad "${net}")
  file(WRITE ${OUT}/scan_bad.v "${bad}")
  simulate("scan_edt_pat_tb: FAIL" masked/scan_edt_pat_tb.sv masked/scan_edt.sv scan_bad.v ${T}/generic_cells.v)
endif()

# 4. Errors.
execute_process(COMMAND ${MDFT} -edt_encode -scandef ${SD} -edt_cubes cubes.txt -edt_xmask -edt_channels 2
                WORKING_DIRECTORY ${OUT} RESULT_VARIABLE rc ERROR_VARIABLE err)
if(NOT rc EQUAL 2 OR NOT err MATCHES "needs more chains than channels")
  message(FATAL_ERROR "-edt_xmask with nothing to mask: rc=${rc} ${err}")
endif()
execute_process(COMMAND ${MDFT} -edt_encode -scandef ${SD} -edt_cubes cubes.txt ${T}/c17.v
                WORKING_DIRECTORY ${OUT} RESULT_VARIABLE rc ERROR_VARIABLE err)
if(NOT rc EQUAL 2)
  message(FATAL_ERROR "a netlist without the chains: rc=${rc} ${err}")
endif()
message(STATUS "mdft_edt_resp: ok")
