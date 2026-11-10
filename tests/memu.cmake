# memu self-test: gate-level netlists are put on one or more FPGAs, and the
# testbench memu writes runs the netlist and its emulation side by side in
# msim; then the transactors, against the same test in plain simulation.
#   cmake -DMEMU=<memu> [-DMSIM=<msim>] -DSRC=<source dir> -DOUT=<dir> -P memu.cmake
# Without MSIM only memu itself runs (a build without slang).
file(REMOVE_RECURSE ${OUT})
file(MAKE_DIRECTORY ${OUT})
set(T ${SRC}/tests/emu)
set(lib ${SRC}/tests/sta/sta_test.lib)
set(xtor ${SRC}/emu/sv/memu_xtor.sv)

function(memu dir)
  execute_process(COMMAND ${MEMU} -lib ${lib} -mem ${T}/mems.txt -o ${OUT}/${dir} -q ${ARGN}
                  WORKING_DIRECTORY ${OUT} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${dir}: memu exited ${rc}\n${out}${err}")
  endif()
endfunction()

function(msim what)
  execute_process(COMMAND ${CMAKE_COMMAND} -E env MVP_RAM_SIZE=0x1000 MVP_UART_BASE=0x1000
                          MVP_UART_BAUD=10000000 ${MSIM} ${ARGN}
                  WORKING_DIRECTORY ${OUT} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${what}: msim exited ${rc}\n${out}${err}")
  endif()
  set(out "${out}" PARENT_SCOPE)
endfunction()

# memu, then the netlist against the emulation on 2000 random events.
function(emulate dir top)
  memu(${dir} ${T}/${top}.gv ${ARGN})
  if(NOT MSIM)
    return()
  endif()
  file(GLOB fpgas ${OUT}/${dir}/${top}_fpga*.v)
  msim(${dir} ${T}/cells.v ${T}/mems.v ${T}/${top}.gv ${fpgas} ${OUT}/${dir}/${top}_emu_lib.v
       ${OUT}/${dir}/${top}_emu.v ${OUT}/${dir}/${top}_emu_tb.sv -top ${top}_emu_tb ${PLUS})
  if(NOT out MATCHES "memu: PASS, 2000 events")
    message(FATAL_ERROR "${dir}: the emulation differs from the netlist\n${out}")
  endif()
endfunction()

function(expect_report dir top)
  file(READ ${OUT}/${dir}/${top}_emu.rpt rpt)
  foreach(want ${ARGN})
    string(FIND "${rpt}" "${want}" pos)
    if(pos EQUAL -1)
      message(FATAL_ERROR "${dir}: the report lacks '${want}':\n${rpt}")
    endif()
  endforeach()
endfunction()

# Gated, divided and falling-edge clocks, an asynchronous reset, a block RAM
# and a LUT RAM: on one FPGA, split, split with too few wires (time-division
# multiplexing), and on a board where two of the FPGAs only meet through the third.
emulate(clocks1 clocks)
expect_report(clocks1 clocks "edge detection" "block RAM" "LUT RAM")
emulate(clocks2 clocks -fpgas 2 -lut 12 -pins 64)
emulate(clocks3 clocks -fpgas 3 -lut 8 -pins 4)
expect_report(clocks3 clocks "multiplexed")
file(WRITE ${OUT}/line.board "board line\nutil 1.0\ntdm_max 8\nfpga A lut=6 ff=9\nfpga B lut=6 ff=9\nfpga C lut=6 ff=9\n"
     "link A B pins=8\nlink B C pins=8\n")
set(PLUS +trace=${OUT}/ring.samples)
emulate(ring clocks -board ${OUT}/line.board -probe "q*" -trace-depth 256)
set(PLUS)
expect_report(ring clocks "probes: 8 nets")
if(MSIM)
  # The probes' samples become a waveform.
  execute_process(COMMAND ${MEMU} -trace ${OUT}/ring/clocks_probes.txt ${OUT}/ring.samples -o ${OUT}/ring.vcd
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "memu -trace exited ${rc}\n${out}${err}")
  endif()
  file(READ ${OUT}/ring.vcd vcd)
  if(NOT vcd MATCHES "\\$var" OR NOT vcd MATCHES "\n#[1-9]")
    message(FATAL_ERROR "the probe waveform is empty:\n${vcd}")
  endif()
endif()

# One clock, rising edges only: one emu_clk cycle per clock cycle.
emulate(sync1 sync)
expect_report(sync1 sync "strobes" "1 emu_clk cycle per event")
emulate(sync2 sync -fpgas 2 -lut 70 -pins 16)
emulate(syncedge sync -edge)
# Two unrelated clocks; a clockless-read memory too big for LUT RAM.
emulate(twoclk1 twoclk -clock clka:10 -clock clkb:14:3)
emulate(twoclk2 twoclk -fpgas 2 -lut 20 -clock clka:10 -clock clkb:30 -lutram-max 64)
expect_report(twoclk2 twoclk "block RAM read on emu_clk")
# A latch-based clock gate.
emulate(icg icg)

# The same answer whatever the number of threads.
memu(t1 ${T}/clocks.gv -fpgas 4 -lut 6 -pins 6 -threads 1)
memu(t8 ${T}/clocks.gv -fpgas 4 -lut 6 -pins 6 -threads 8)
foreach(f clocks_fpga0.v clocks_fpga3.v clocks_emu.v clocks_emu_lib.v clocks_emu_tb.sv)
  execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files ${OUT}/t1/${f} ${OUT}/t8/${f} RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${f} differs between -threads 1 and -threads 8")
  endif()
endforeach()

# What cannot work is refused, with the reason.
function(refuse what want)
  execute_process(COMMAND ${MEMU} -o ${OUT}/refused -q ${ARGN}
                  WORKING_DIRECTORY ${OUT} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(rc EQUAL 0 OR NOT err MATCHES "${want}")
    message(FATAL_ERROR "${what}: expected an error matching '${want}', got exit ${rc}\n${out}${err}")
  endif()
endfunction()
refuse("too small a board" "fit" ${T}/clocks.gv -lib ${lib} -mem ${T}/mems.txt -fpgas 2 -lut 4)
refuse("an unknown cell" "SRAM16X8" ${T}/clocks.gv -lib ${lib})
refuse("no library" "lib" ${T}/sync.gv)

# What memu writes is for FPGA synthesis: where Yosys is installed, it must
# take an FPGA's netlist (with the cells' Verilog models) and the transactors.
find_program(YOSYS NAMES yosys yowasp-yosys)
if(YOSYS)
  function(synth what top)
    string(REPLACE ";" " " files "${ARGN}")
    execute_process(COMMAND ${YOSYS} -q -p "read_verilog -sv -DSYNTHESIS ${files}; synth -lut 4 -top ${top}; check -assert"
                    WORKING_DIRECTORY ${OUT} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT rc EQUAL 0 OR "${out}${err}" MATCHES "ERROR")
      message(FATAL_ERROR "${what}: Yosys refused it (exit ${rc})\n${out}${err}")
    endif()
  endfunction()
  synth("an FPGA of clocks3" clocks_fpga2 ${T}/cells.v ${T}/mems.v clocks3/clocks_fpga2.v
        clocks3/clocks_emu_lib.v)
  synth("the slave transactor" memu_axil_slave_xtor ${xtor})
  synth("the master transactor" memu_axil_master_xtor ${xtor})
  message(STATUS "memu: Yosys synthesizes the output")
endif()

if(NOT MSIM)
  message(STATUS "memu: ok (memu only: no msim to run the emulation in)")
  return()
endif()

# Transactors. The design is the bus master and the msim -mvp models answer:
# simulated with mvp_axil_slave, then emulated with the transactor in its
# place. The same cycle counts, the same data, the same interrupt.
set(mvp ${SRC}/mvp)
set(models ${mvp}/models/ram.c ${mvp}/models/uart.c)
msim("xsoc simulated" ${mvp}/sv/mvp.sv +define+GOLD ${T}/xsoc.v ${T}/xsoc_tb.sv ${mvp}/mvp.c ${models} -top xsoc_tb)
string(REGEX MATCH "xsoc: done[^\n]*" gold "${out}")
if(NOT gold MATCHES "waited [1-9]")
  message(FATAL_ERROR "xsoc simulated: no result\n${out}")
endif()
foreach(cfg "x1" "x2;-fpgas;2;-lut;90;-pins;24")
  list(GET cfg 0 dir)
  list(REMOVE_AT cfg 0)
  memu(${dir} ${T}/xsoc.gv ${cfg})
  expect_report(${dir} xsoc "units of 5000 ps")
  file(GLOB fpgas ${OUT}/${dir}/xsoc_fpga*.v)
  msim("xsoc emulated (${dir})" ${mvp}/sv/mvp.sv +define+UNIT_PS=5000 ${T}/cells.v ${fpgas}
       ${OUT}/${dir}/xsoc_emu_lib.v ${OUT}/${dir}/xsoc_emu.v ${xtor} ${SRC}/emu/sv/memu_mvp_host.sv
       ${T}/xsoc_tb.sv ${mvp}/mvp.c ${models} -top xsoc_tb)
  string(REGEX MATCH "xsoc: done[^\n]*" emu "${out}")
  if(NOT emu STREQUAL gold)
    message(FATAL_ERROR "xsoc (${dir}): simulated '${gold}', emulated '${emu}'\n${out}")
  endif()
endforeach()

# The host is the bus master: a class with a virtual interface drives the
# design through the transactor, simulated and emulated, to the same log.
msim("xreg simulated" +define+GOLD ${T}/xreg.v ${xtor} ${T}/xreg_tb.sv -top xreg_tb)
set(gold "${out}")
if(NOT gold MATCHES "xreg: PASS")
  message(FATAL_ERROR "xreg simulated: failed\n${out}")
endif()
foreach(cfg "r1" "r3;-fpgas;3;-lut;220;-pins;16")
  list(GET cfg 0 dir)
  list(REMOVE_AT cfg 0)
  memu(${dir} ${T}/xreg.gv ${cfg})
  file(GLOB fpgas ${OUT}/${dir}/xreg_fpga*.v)
  msim("xreg emulated (${dir})" ${T}/cells.v ${fpgas} ${OUT}/${dir}/xreg_emu_lib.v ${OUT}/${dir}/xreg_emu.v
       ${xtor} ${T}/xreg_tb.sv -top xreg_tb)
  if(NOT out STREQUAL gold)
    message(FATAL_ERROR "xreg (${dir}): the emulated run's log differs\n--- simulated\n${gold}--- emulated\n${out}")
  endif()
endforeach()

message(STATUS "memu: ok")
