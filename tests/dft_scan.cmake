# mdft -insert_scan self-test: stitch tests/dft/scan_top.v in several ways and
# simulate each result with msim: the chain flush testbench must pass, and with
# scan_en low the netlist must still behave like the original.
#   cmake -DMDFT=<mdft> -DMSIM=<msim> -DSRC=<source dir> -DOUT=<scratch dir> -P dft_scan.cmake
file(MAKE_DIRECTORY ${OUT})
set(D ${SRC}/tests/dft)

function(mdft_ok)
  execute_process(COMMAND ${MDFT} -insert_scan ${ARGN}
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  set(out "${out}${err}" PARENT_SCOPE)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "mdft -insert_scan ${ARGN} exited ${rc}\n${out}${err}")
  endif()
endfunction()
function(sim want)
  execute_process(COMMAND ${MSIM} ${ARGN} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT out MATCHES "${want}")
    message(FATAL_ERROR "msim ${ARGN}: expected '${want}'\n${out}${err}")
  endif()
endfunction()
function(want_in text)
  foreach(w ${ARGN})
    string(FIND "${text}" "${w}" pos)
    if(pos EQUAL -1)
      message(FATAL_ERROR "missing: ${w}\n${text}")
    endif()
  endforeach()
endfunction()

# Every way of cutting the chains shifts correctly.
foreach(variant "default:" "two:-mix_clocks;-chains;2" "one:-mix_clocks" "six:-chains;6" "short:-max_length;2")
  string(REGEX MATCH "^[^:]*" name "${variant}")
  string(REGEX REPLACE "^[^:]*:" "" opts "${variant}")
  mdft_ok(${D}/scan_top.v -top top -o ${OUT}/${name}.v -tb ${OUT}/${name}_tb.v -scandef ${OUT}/${name}.json
          -models ${OUT}/${name}_models.v ${opts})
  sim("SCAN FLUSH PASS" ${OUT}/${name}.v ${D}/scan_cells.v ${OUT}/${name}_tb.v -top top_scan_tb)
endforeach()

# The default: one chain per clock domain, the falling-edge flop on its own,
# the divided-clock flop left out with the reason, the clock gate held open.
mdft_ok(${D}/scan_top.v -top top -o ${OUT}/default.v -scandef ${OUT}/default.json)
want_in("${out}"
  "8 of 9 flops scanned (88%), 3 chains"
  "chain0: 1 flop, scan_in[0] -> scan_out[0], clock clk (falling)"
  "chain1: 5 flops, scan_in[1] -> scan_out[1], clock clk"
  "chain2: 2 flops, scan_in[2] -> scan_out[2], clock clk2"
  "0 swapped for scan cells, 8 given a mux"
  "1 clock gate held open by scan_en"
  "not scanned: div_reg: clock not controllable: 'cnt[0]' is driven by flop 'cnt_reg_0_' (DFFR)")
file(READ ${OUT}/default.v net)
want_in("${net}"
  "MUX2 cnt_reg_1__scan_mux (.A(n1), .B(cnt[0]), .S(scan_en), .Y(cnt_reg_1__scan_d));"
  "ICG g0 (.CK(clk), .E(en), .TE(scan_en), .GCK(gclk));"
  "assign scan_out[1] = x;")
file(READ ${OUT}/default.json def)
want_in("${def}"
  "\"scan_enable\": {\"port\": \"scan_en\", \"active\": 1}"
  "{\"port\": \"rst_n\", \"active\": 0}"
  "{\"inst\": \"u_sub/u_b\", \"cell\": \"DFF\", \"clock\": \"clk2\", \"edge\": \"pos\", \"q\": \"q2\", \"scan\": \"mux\"}"
  "{\"inst\": \"div_reg\", \"reason\": \"clock not controllable")

# Scan off: the same behaviour as before insertion.
file(READ ${OUT}/default.v net)
string(REPLACE "module top (" "module top_scan (" net "${net}")
file(WRITE ${OUT}/default_renamed.v "${net}")
sim("SCAN EQ PASS" ${D}/scan_top.v ${OUT}/default_renamed.v ${D}/scan_cells.v ${D}/scan_eq_tb.v -top scan_eq_tb)

# One chain across three domains: a lockup latch where the clock changes.
mdft_ok(${D}/scan_top.v -o ${OUT}/one.v -mix_clocks)
want_in("${out}" "chain0: 8 flops, scan_in -> scan_out, clock clk, clk (falling), clk2, 1 lockup latch")
file(READ ${OUT}/one.v net)
want_in("${net}" "LATCHN mdft_lockup_0 (.D(x), .GN(clkb), .Q(mdft_lockup_0_q));")

# With a Liberty library: scan cells come from test_cell, the flop without a
# scan equivalent gets the library's mux, the lockup its latch.
mdft_ok(${D}/scan_top.v -lib ${D}/scan.lib -o ${OUT}/lib.v -tb ${OUT}/lib_tb.v -mix_clocks)
want_in("${out}" "7 swapped for scan cells, 1 given a mux")
file(READ ${OUT}/lib.v net)
want_in("${net}"
  "SDFFR cnt_reg_1_ (.D(n1), .CK(clk), .RN(rst_n), .Q(cnt[1]), .SI(cnt[0]), .SE(scan_en));"
  "MUX2 neg_reg_scan_mux"
  "LATCH mdft_lockup_0 (.D(x), .G(mdft_lockup_0_g), .Q(mdft_lockup_0_q));")
sim("SCAN FLUSH PASS" ${OUT}/lib.v ${D}/scan_cells.v ${OUT}/lib_tb.v -top top_scan_tb)

# A cell map overrides the library; -exclude leaves flops out; the port names
# can be changed.
file(WRITE ${OUT}/map.txt "flop DFF D=D CK=CK Q=Q scan=SDFF\nscanflop SDFF D=D SI=SI SE=SE CK=CK Q=Q\n")
mdft_ok(${D}/scan_top.v -cellmap ${OUT}/map.txt -exclude "cnt_reg_*" -o ${OUT}/map.v -tb ${OUT}/map_tb.v
        -scan_in si -scan_out so -scan_en se -mix_clocks)
want_in("${out}" "4 of 9 flops scanned" "3 swapped for scan cells, 1 given a mux" "si -> so"
        "not scanned: cnt_reg_0_: excluded with -exclude")
sim("SCAN FLUSH PASS" ${OUT}/map.v ${D}/scan_cells.v ${OUT}/map_tb.v -top top_scan_tb)

# The result does not depend on the thread count.
mdft_ok(${D}/scan_top.v -o ${OUT}/t1.v -scandef ${OUT}/t1.json -chains 4 -threads 1 -quiet)
mdft_ok(${D}/scan_top.v -o ${OUT}/t8.v -scandef ${OUT}/t8.json -chains 4 -threads 8 -quiet)
foreach(f v json)
  file(READ ${OUT}/t1.${f} a)
  file(READ ${OUT}/t8.${f} b)
  if(NOT a STREQUAL b)
    message(FATAL_ERROR "-threads 1 and -threads 8 differ in the .${f} output")
  endif()
endforeach()

# Errors are errors.
foreach(bad "-scan_in;clk" "-style;cell" "-chains;0" "-top;nope")
  execute_process(COMMAND ${MDFT} -insert_scan ${D}/scan_top.v -o ${OUT}/bad.v ${bad}
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(rc EQUAL 0 OR NOT err MATCHES "^mdft: ")
    message(FATAL_ERROR "mdft -insert_scan ${bad} should fail: rc=${rc}\n${out}${err}")
  endif()
endforeach()
message(STATUS "mdft -insert_scan: ok")
