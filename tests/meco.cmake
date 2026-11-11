# meco self-test: each case in tests/eco is patched, must be proved
# equivalent, must report the expected patch, and must give the same patched
# netlist and script on one thread and on four.
#   cmake -DMECO=<meco> -DSRC=<source dir> -DOUT=<scratch dir> -P meco.cmake
file(MAKE_DIRECTORY ${OUT})
set(LIBS -lib ${SRC}/tests/sta/sta_test.lib -lib ${SRC}/tests/eco/eco_cells.lib)

function(eco name args)
  foreach(t 1 4)
    execute_process(COMMAND ${MECO} -old ${SRC}/tests/eco/${name}_old.v -new ${SRC}/tests/eco/${name}_new.v ${LIBS}
                            ${args} -threads ${t} -o ${OUT}/${name}_${t}.v -script ${OUT}/${name}_${t}.tcl
                    RESULT_VARIABLE rc OUTPUT_VARIABLE out${t} ERROR_VARIABLE err)
    if(NOT rc EQUAL 0)
      message(FATAL_ERROR "meco ${name} -threads ${t} exited ${rc}\n${out${t}}${err}")
    endif()
  endforeach()
  foreach(ext v tcl)
    file(READ ${OUT}/${name}_1.${ext} a)
    file(READ ${OUT}/${name}_4.${ext} b)
    if(NOT a STREQUAL b)
      message(FATAL_ERROR "meco ${name}: ${ext} output differs between -threads 1 and -threads 4")
    endif()
  endforeach()
  if(NOT out1 STREQUAL out4)
    message(FATAL_ERROR "meco ${name}: report differs between -threads 1 and -threads 4\n${out1}\n${out4}")
  endif()
  foreach(want ${ARGN})
    string(FIND "${out1}" "${want}" pos)
    if(pos EQUAL -1)
      message(FATAL_ERROR "meco ${name}: missing '${want}' in\n${out1}")
    endif()
  endforeach()
  # The patched netlist against the new one again: nothing left to change.
  execute_process(COMMAND ${MECO} -old ${OUT}/${name}_1.v -new ${SRC}/tests/eco/${name}_new.v ${LIBS} ${args} -q
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT rc EQUAL 0 OR NOT out MATCHES " 0 changed")
    message(FATAL_ERROR "meco ${name}: the patched netlist still differs from the new one\n${out}${err}")
  endif()
endfunction()

# Scan flops against plain flops (scan enable tied off), two outputs and one
# register input changed; the spare cell and the scan chain stay.
eco(scan "-tie;scan_en=0"
    "10 compare points, 3 changed" "+3 cells" "-3 cells" "proved equivalent"
    "+ cell ECO_g3 (OR2_X1)" "> r0/D <- ECO_m3")
file(READ ${OUT}/scan_1.v text)
foreach(want "NAND2_X1 spare0" "SDFFR_X1 r1 (.CK(cclk), .D(n2), .SI(q0), .SE(scan_en)" "assign scan_out = q1")
  string(FIND "${text}" "${want}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "scan: the patched netlist lost '${want}'\n${text}")
  endif()
endforeach()

# One gate changed deep in the logic: 14 compare points differ, one cell fixes them.
eco(deep "" "14 changed" "+1 cells" "-1 cells" "+ cell ECO_cy2 (OR2_X1)" "proved equivalent")
# Without the search for the changed gate the whole fanout is copied.
execute_process(COMMAND ${MECO} -old ${SRC}/tests/eco/deep_old.v -new ${SRC}/tests/eco/deep_new.v ${LIBS} -no-rectify -q
                RESULT_VARIABLE rc OUTPUT_VARIABLE out)
if(NOT rc EQUAL 0 OR NOT out MATCHES "proved equivalent" OR out MATCHES "\\+1 cells")
  message(FATAL_ERROR "deep -no-rectify: ${rc}\n${out}")
endif()

# A register swapped for another cell (its QN rebuilt with an inverter), one
# removed, one added, new input and output ports.
eco(regs "" "r2 (new)" "r1 (removed)" "+ output port w" "+ input port c" "+ input port en"
    "+ cell r0 (DFFE_X1)" "+ cell r2 (DFF_X1)" "- cell r1 (DFF_X1)" "+ cell ECO_inv (INV_X1)" "proved equivalent")

# A hierarchical old netlist is patched flat.
eco(hier "" "s1/r.next" "+ cell ECO_s1/g (OR2_X1)" "- cell s1/g (AND2_X1)" "proved equivalent")

# Black boxes: a memory pin and one bit of a bus pin.
eco(box "" "mem/WE" "r/D[3]" "+1 cells" "-2 cells" "proved equivalent")
file(READ ${OUT}/box_1.v text)
string(FIND "${text}" "REG4_X1 r (.CK(clk), .D({ECO_x, d[2], d[1], d[0]}), .Q(q));" pos)
if(pos EQUAL -1)
  message(FATAL_ERROR "box: bus pin not rewired as expected\n${text}")
endif()

# Metal-only: no cell added or removed. The OR2 the fix needs comes from a
# spare MUX2 (S and B on one net), the AND2 it replaces is left as a spare
# with its inputs tied low.
eco(spare "-spare-cells;spare_*" "+0 cells" "-0 cells" "1 spare cells used, 1 old cells left as spares, 0 cells without a spare"
    "> spare_m0/S <- a[2]" "> spare_m0/Y <- ECO_c[2]" "> cy2/A <- 1'b0" "proved equivalent")
# Only a NOR2 and a NAND2 are spare: OR2 is the NOR2 with the NAND2 as its inverter.
eco(spare2 "-spare" "+0 cells" "2 spare cells used" "> spare_n0/A <- ECO_n" "> spare_n0/B <- ECO_n" "proved equivalent")
file(READ ${OUT}/spare2_1.v text)
string(FIND "${text}" "NOR2_X1 spare_r0 (.A(c[1]), .B(a[2]), .Y(ECO_n));" pos)
if(pos EQUAL -1)
  message(FATAL_ERROR "spare2: spare cell not wired as expected\n${text}")
endif()
# No spare cell at all: the cell is added, and the exit status says so.
execute_process(COMMAND ${MECO} -old ${SRC}/tests/eco/deep_old.v -new ${SRC}/tests/eco/deep_new.v ${LIBS} -spare
                RESULT_VARIABLE rc OUTPUT_VARIABLE out)
if(NOT rc EQUAL 3 OR NOT out MATCHES "no spare cell for ECO_cy2 \\(OR2_X1\\)" OR NOT out MATCHES "proved equivalent")
  message(FATAL_ERROR "deep -spare: ${rc}\n${out}")
endif()

# Nothing to do: the same design, and the exit status of a netlist that cannot be read.
execute_process(COMMAND ${MECO} -old ${SRC}/tests/eco/deep_new.v -new ${SRC}/tests/eco/deep_new.v ${LIBS} -o ${OUT}/same.v
                RESULT_VARIABLE rc OUTPUT_VARIABLE out)
if(NOT rc EQUAL 0 OR NOT out MATCHES " 0 changed; patch: \\+0 cells")
  message(FATAL_ERROR "same design: ${rc}\n${out}")
endif()
execute_process(COMMAND ${MECO} -old ${SRC}/tests/eco/nope.v -new ${SRC}/tests/eco/deep_new.v ${LIBS}
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 2 OR NOT err MATCHES "cannot open")
  message(FATAL_ERROR "missing file: ${rc}\n${out}${err}")
endif()
