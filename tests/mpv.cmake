# mpv drc self-test: SKY130 standard cells are clean under tests/pv/sky130.drc,
# a layout with known violations reports each of them, and the report is the
# same on one thread and on many.
#   cmake -DMPV=<mpv> -DGEN=<test_pv> -DSRC=<source dir> -DOUT=<scratch dir> -P mpv.cmake
file(MAKE_DIRECTORY ${OUT})

function(run want_rc)
  execute_process(COMMAND ${MPV} ${ARGN} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  set(out "${out}" PARENT_SCOPE)
  set(err "${err}" PARENT_SCOPE)
  if(NOT rc EQUAL ${want_rc})
    message(FATAL_ERROR "mpv ${ARGN} exited ${rc}, expected ${want_rc}\n${out}${err}")
  endif()
endfunction()

function(expect text)
  string(FIND "${out}" "${text}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing: ${text}\n${out}")
  endif()
endfunction()

# Real cells: clean.
foreach(cell inv_1 nand2_1 mux2_1 dfxtp_1)
  run(0 drc -deck ${SRC}/tests/pv/sky130.drc ${SRC}/tests/pv/sky130/sky130_fd_sc_hd__${cell}.gds)
  expect("cell sky130_fd_sc_hd__${cell}")
  expect("0 violations in 0 of 35 rules")
endforeach()

# The same cell against a tighter rule: the tool does report what is there.
file(READ ${SRC}/tests/pv/sky130.drc deck)
string(REPLACE "width(li) < 0.17" "width(li) < 0.18" deck "${deck}")
file(WRITE ${OUT}/tight.drc "${deck}")
run(1 drc -deck ${OUT}/tight.drc ${SRC}/tests/pv/sky130/sky130_fd_sc_hd__inv_1.gds -q)
expect("li.1")
if(out MATCHES "poly.1a")
  message(FATAL_ERROR "-q printed a clean rule:\n${out}")
endif()

# Known violations, with a cell reference and a path in the layout.
execute_process(COMMAND ${GEN} -gen ${OUT} RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "test_pv -gen failed")
endif()
run(1 drc -deck ${SRC}/tests/pv/mini.drc ${OUT}/pv_bad.gds -o ${OUT}/bad.txt -threads 1)
set(one "${out}")
expect("cell bad, 6 shapes on 3 layers, 5 x 5 um, 1 unit = 0.001 um")
expect("M.W         1  metal width")
expect("M.S         1  metal spacing")
expect("M.A         1  metal area")
expect("V.SZ        0  via must be 0.17 x 0.17")
expect("V.E         1  metal enclosure of via")
expect("M.D         1  metal density in 5 um windows")
expect("5 violations in 5 of 6 rules")
file(READ ${OUT}/bad.txt out)
expect("rule M.W 1 \"metal width\"\n  0 0.42 2 0.52 0.1\n")
expect("rule M.S 1 \"metal spacing\"\n  0 0.3 2 0.42 0.12\n")
expect("rule M.A 1 \"metal area\"\n  3 2.9 3.4 3.1 0.08\n")
expect("rule V.E 1 \"metal enclosure of via\"\n  1 0 1.17 0.02 0.02\n")
expect("rule M.D 1 \"metal density in 5 um windows\"\n  0 0 5 5 0.0352\n")
run(1 drc -deck ${SRC}/tests/pv/mini.drc ${OUT}/pv_bad.gds -threads 4)
if(NOT out STREQUAL one)
  message(FATAL_ERROR "-threads 4 and -threads 1 differ:\n${one}\n---\n${out}")
endif()

# Errors: exit 2 with the reason.
run(2 drc -deck ${SRC}/tests/pv/mini.drc ${OUT}/pv_bad.gds -top nosuch)
if(NOT err MATCHES "no cell named nosuch")
  message(FATAL_ERROR "bad -top: ${err}")
endif()
run(2 drc -deck ${SRC}/tests/pv/mini.drc ${SRC}/tests/pv/mini.drc)
if(NOT err MATCHES "not a GDSII stream|bad record|truncated")
  message(FATAL_ERROR "not a GDS file: ${err}")
endif()
file(WRITE ${OUT}/broken.drc "layer m 1\nrule r width(m < 0.1\n")
run(2 drc -deck ${OUT}/broken.drc ${OUT}/pv_bad.gds)
if(NOT err MATCHES "broken.drc: line 2: expected \\) in width")
  message(FATAL_ERROR "deck error: ${err}")
endif()

# LVS extraction: the flop has 24 transistors, as in the PDK netlist.
run(0 lvs -deck ${SRC}/tests/pv/sky130.lvs ${SRC}/tests/pv/sky130/sky130_fd_sc_hd__dfxtp_1.gds -o ${OUT}/dfxtp.spice)
expect("cell sky130_fd_sc_hd__dfxtp_1, 24 devices, 18 nets, 7 ports")
expect("sky130_fd_pr__nfet_01v8          12")
file(READ ${OUT}/dfxtp.spice out)
expect(".subckt sky130_fd_sc_hd__dfxtp_1 CLK D Q VGND VNB VPB VPWR\n")
expect("M22 VPWR n14 n16 VPB sky130_fd_pr__pfet_01v8_hvt w=1 l=0.15\n")
run(2 lvs -deck ${SRC}/tests/pv/mini.drc ${OUT}/pv_bad.gds)

# A routed DEF with its LEF, rules from the LEF technology: the metal is
# clean, and two pairs of v2 cuts are closer than the cut spacing.
run(1 drc -lef ${SRC}/tests/pv/def/tiny.lef ${SRC}/tests/pv/def/counter.def -write-deck ${OUT}/tiny.drc -o ${OUT}/counter.txt)
expect("cell counter, 607 shapes on 7 layers, 17.1 x 18.2 um, 1 unit = 0.0005 um")
expect("m2.space        0  m2 spacing")
expect("v2.space        2  v2 spacing")
expect("2 violations in 1 of 27 rules")
file(READ ${OUT}/tiny.drc out)
expect("layer m1 1/0\n")
expect("rule m1.width width(m1) < 0.07 \"m1 width\"\n")
expect("rule v1.above v1 - m2 \"v1 without m2\"\n")
# The written deck gives the same result when passed back with -deck.
run(1 drc -lef ${SRC}/tests/pv/def/tiny.lef -deck ${OUT}/tiny.drc ${SRC}/tests/pv/def/counter.def -q)
expect("2 violations in 1 of 27 rules")
run(2 drc -deck ${OUT}/tiny.drc ${SRC}/tests/pv/def/counter.def)
