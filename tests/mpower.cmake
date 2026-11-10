# mpower self-test.
#   cmake -DMPOWER=<mpower> -DMSIM=<msim> -DSRC=<source dir> -DOUT=<work dir> -DFST=<ON|OFF> -P mpower.cmake
#
# The hand-checked case is tests/power/gates.v with tests/power/gates.saif
# (1000 ns; clk 200 toggles, a 20, b 10, n1 20, n2 16, y 8) on tests/power/pw.lib
# at 1 V. Energies are in pJ and capacitances in pF, so pJ per 1000 ns reads directly as uW:
#   r  (DFF)   CK pin (0.020 + 0.018) / 2 * 200 toggles         = 3.8 pJ   -> 3.800 uW
#              Q      (0.030 + 0.026) / 2 * 8                   = 0.224    -> 0.224 uW
#              leakage 10 nW
#   u1 (INV)   Y      (0.006 + 0.004) / 2 * 20                  = 0.100 uW
#              net n1 0.5 * 0.002 pF * 1 V^2 * 20               = 0.020 uW
#              leakage 4 nW * P(A) + 2 nW * P(!A) = 4*0.4 + 2*0.6 = 2.8 nW
#   u2 (NAND2) Y      arcs weighted by the related pin's toggles (n1 20, b 10):
#                     rise (0.010*20 + 0.014*10) / 30, fall (0.008*20 + 0.012*10) / 30,
#                     mean 0.0103333 * 16                       = 0.16533 uW
#              net n2 0.5 * 0.002 * 16                          = 0.016 uW
#              leakage 3 nW
#   clk net    0.5 * 0.003 * 200 = 0.300 uW (clock_network); a and b nets 0.020 + 0.010 (primary_inputs)
#   total      internal 4.28933, switching 0.366, leakage 0.0158 -> 4.67113 uW
set(P ${SRC}/tests/power)
file(REMOVE_RECURSE ${OUT})
file(MAKE_DIRECTORY ${OUT})

function(run_tool tool)
  execute_process(COMMAND ${tool} ${ARGN} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  set(out "${out}" PARENT_SCOPE)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${tool} ${ARGN} exited ${rc}\n${out}${err}")
  endif()
endfunction()
function(want text)
  string(FIND "${out}" "${text}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing: ${text}\n${out}")
  endif()
endfunction()
# "total": {"internal": .., "switching": .., "leakage": .., "total": X} of a JSON report, in watts.
function(json_total file var)
  file(READ ${file} j)
  string(JSON v GET "${j}" total total)
  set(${var} "${v}" PARENT_SCOPE)
endfunction()
function(near got expect what)   # CMake has no floating point: the leading digits must match
  if(NOT "${got}" MATCHES "^${expect}")
    message(FATAL_ERROR "${what}: got ${got}, expected ${expect}...")
  endif()
endfunction()

# 1. The hand-checked numbers, from SAIF.
run_tool(${MPOWER} ${P}/gates.v -top dut -lib ${P}/pw.lib -saif ${P}/gates.saif -strip_path tb/dut
         -cells 3 -hier 1 -json ${OUT}/hand.json)
want("design    dut: 3 cells, 6 nets")
want("100.0% of nets annotated (under tb.dut)")
want("clock     100 MHz")
want("clock_network                        0         0.3           0         0.3")
want("register                         4.024           0        0.01       4.034")
want("combinational                   0.2653       0.036      0.0058      0.3071")
want("primary_inputs                       0        0.03           0        0.03")
want("r (DFF)                          4.024           0        0.01       4.034")
want("u2 (NAND2)                      0.1653       0.016       0.003      0.1843")
want("u1 (INV)                           0.1        0.02      0.0028      0.1228")
want("total power 4.671 uW (internal 4.289, switching 0.366, leakage 0.0158)")
json_total(${OUT}/hand.json t)
near("${t}" "4.6711[23]" "hand-checked total")
# Without -strip_path the design's scope in the activity file is found.
run_tool(${MPOWER} ${P}/gates.v -top dut -lib ${P}/pw.lib -saif ${P}/gates.saif -q)
want("total power 4.671 uW")
# With the cells' Verilog models in the design, the cells are still library cells.
run_tool(${MPOWER} ${P}/cells.v ${P}/gates.v -top dut -lib ${P}/pw.lib -saif ${P}/gates.saif -q)
want("total power 4.671 uW")
# Voltage: dynamic power goes with V^2 (4.65533 * 0.81), leakage with V (0.0158 * 0.9).
run_tool(${MPOWER} ${P}/gates.v -top dut -lib ${P}/pw.lib -saif ${P}/gates.saif -voltage 0.9 -q)
want("total power 3.785 uW (internal 3.474, switching 0.2965, leakage 0.01422)")
# Wire capacitance per fanout: 1 fF on each of the 5 loaded nets' sinks.
run_tool(${MPOWER} ${P}/gates.v -top dut -lib ${P}/pw.lib -saif ${P}/gates.saif -wire_cap 1 -q)
want("switching 0.499,")

# 2. Simulate, and take the activity as VCD, as SAIF (msim -saif) and as FST: one answer.
run_tool(${MSIM} ${P}/cells.v ${P}/gates.v ${P}/tb.sv -top tb -vcd ${OUT}/g.vcd)
run_tool(${MSIM} ${P}/cells.v ${P}/gates.v ${P}/tb.sv -top tb -saif ${OUT}/g.saif)
set(acts g.vcd g.saif)
if(FST)
  run_tool(${MSIM} ${P}/cells.v ${P}/gates.v ${P}/tb.sv -top tb -fst ${OUT}/g.fst)
  list(APPEND acts g.fst)
endif()
foreach(a ${acts})
  run_tool(${MPOWER} ${P}/gates.v -top dut -lib ${P}/pw.lib -activity ${OUT}/${a} -strip_path tb.dut -q)
  want("total power 5.432 uW (internal 4.983, switching 0.4325, leakage 0.01645)")
endforeach()
# mpower converts a waveform to the SAIF msim writes.
run_tool(${MPOWER} -vcd ${OUT}/g.vcd -write_saif ${OUT}/conv.saif)
file(READ ${OUT}/conv.saif a)
file(READ ${OUT}/g.saif b)
string(REGEX REPLACE "\\((DESIGN|PROGRAM_NAME)[^\n]*\n" "" a "${a}")
string(REGEX REPLACE "\\((DESIGN|PROGRAM_NAME)[^\n]*\n" "" b "${b}")
if(NOT a STREQUAL b)
  message(FATAL_ERROR "mpower -write_saif and msim -saif differ")
endif()
if(NOT a MATCHES "\\(TIMESCALE 1 ps\\)\n\\(DURATION 400000\\)" OR
   NOT a MATCHES "\\(clk \\(T0 200000\\) \\(T1 200000\\) \\(TX 0\\) \\(TC 80\\) \\(IG 0\\)\\)")
  message(FATAL_ERROR "SAIF contents:\n${a}")
endif()

# 3. Peak power: busy for the first 200 ns, idle after; the windows average to the total.
run_tool(${MPOWER} ${P}/gates.v -top dut -lib ${P}/pw.lib -vcd ${OUT}/g.vcd -strip_path tb.dut
         -peak -waveform ${OUT}/w.csv -json ${OUT}/peak.json)
want("peak      8.8 uW at 30 ns (10 ns windows, 40 of them), 1.62x the average")
file(STRINGS ${OUT}/w.csv rows)
list(LENGTH rows n)
if(NOT n EQUAL 41)
  message(FATAL_ERROR "-waveform: ${n} rows, expected 41")
endif()
list(GET rows 40 last)
if(NOT last MATCHES "^390,4.11645e-06$")   # idle: clock pin, clock net and leakage only
  message(FATAL_ERROR "-waveform: last row ${last}")
endif()
execute_process(COMMAND ${MPOWER} ${P}/gates.v -top dut -lib ${P}/pw.lib -saif ${P}/gates.saif -peak
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(rc EQUAL 0 OR NOT err MATCHES "-peak needs a waveform")
  message(FATAL_ERROR "-peak with SAIF should be refused:\n${out}${err}")
endif()

# 4. RTL activity on the gate netlist: n1 is not in the RTL, and gets its activity
#    through the inverter's function; the answer is the gate-level one.
run_tool(${MSIM} ${P}/rtl.sv ${P}/tb.sv -top tb -vcd ${OUT}/r.vcd)
run_tool(${MPOWER} ${P}/gates.v -top dut -lib ${P}/pw.lib -vcd ${OUT}/r.vcd)
want("83.3% of nets annotated (under tb.dut)")
want("total power 5.432 uW (internal 4.983, switching 0.4325, leakage 0.01645)")
# No activity at all: a clock period, and default toggle rates behind it.
run_tool(${MPOWER} ${P}/gates.v -top dut -lib ${P}/pw.lib -clock clk 10)
want("activity  none: propagated from -clock")
want("register                          4.08           0        0.01        4.09")
# Neither: leakage only.
run_tool(${MPOWER} ${P}/gates.v -top dut -lib ${P}/pw.lib -q)
want("total power 16 nW (internal 0, switching 0, leakage 16)")

# 5. RTL estimate: one flop and the logic as reference gates; the register and
#    clock rows are the gate netlist's, since the flop is the same cell.
run_tool(${MPOWER} ${P}/rtl.sv -top dut -lib ${P}/pw.lib -vcd ${OUT}/r.vcd)
want("RTL estimated as 1 flops (DFF) and 2.4 gates (NAND2)")
want("clock_network                        0         0.3           0         0.3")
want("register                           4.5           0        0.01        4.51")
want("note: RTL logic is an estimate")
# and with no library at all, the built-in technology
run_tool(${MPOWER} ${P}/rtl.sv -top dut -vcd ${OUT}/r.vcd)
want("library   mpower_generic, 1 V")
want("note: no -lib given")

# 6. UPF: PD_CORE runs at 0.8 V behind a switch that is on 30% of the time.
#    u_core.r: clock pin 3.8 uW * 0.64 * 0.3 + Q 0.028 pJ * 6 * 0.64; leakage (10 + 3) nW * 0.8 * 0.3.
run_tool(${MPOWER} ${P}/pd.v -top pd -lib ${P}/pw.lib -saif ${P}/pd.saif -upf ${P}/pd.upf -hier 1 -json ${OUT}/pd.json)
want("PD_TOP                            4.46        0.02       0.013       4.493")
want("1 V, always")
want("PD_CORE                         0.8563     0.00384     0.00312      0.8633")
want("0.8 V, 30%")
file(READ ${OUT}/pd.json j)
string(JSON on GET "${j}" domains PD_CORE on_fraction)
string(JSON sw GET "${j}" domains PD_CORE switched)
if(NOT on MATCHES "^0.(3|29999)" OR NOT sw STREQUAL "ON")
  message(FATAL_ERROR "pd.json: on_fraction ${on}, switched ${sw}")
endif()

# 7. The same report on any thread count (a design big enough to be split).
find_program(PY python3)
if(PY)
  run_tool(${PY} ${SRC}/bench/gen_power.py 4000 ${OUT}/big)
  set(ref "")
  foreach(n 1 2 3 8)
    run_tool(${MPOWER} ${OUT}/big.v -top big -lib ${P}/pw.lib -vcd ${OUT}/big.vcd -peak -interval 5
             -hier 1 -cells 5 -threads ${n} -json ${OUT}/big${n}.json)
    file(READ ${OUT}/big${n}.json j)
    if(ref STREQUAL "")
      set(ref "${j}")
    elseif(NOT j STREQUAL ref)
      message(FATAL_ERROR "-threads ${n} gives a different report than -threads 1")
    endif()
  endforeach()
endif()

# 8. Errors are messages, not crashes.
execute_process(COMMAND ${MPOWER} ${P}/gates.v -top dut -lib ${OUT}/none.lib RESULT_VARIABLE rc ERROR_VARIABLE err OUTPUT_QUIET)
if(rc EQUAL 0 OR NOT err MATCHES "cannot open")
  message(FATAL_ERROR "missing library: rc ${rc}: ${err}")
endif()
execute_process(COMMAND ${MPOWER} ${P}/gates.v -top dut -lib ${P}/pw.lib -saif ${OUT}/none.saif RESULT_VARIABLE rc ERROR_VARIABLE err OUTPUT_QUIET)
if(rc EQUAL 0 OR NOT err MATCHES "cannot read")
  message(FATAL_ERROR "missing activity: rc ${rc}: ${err}")
endif()
execute_process(COMMAND ${MPOWER} ${P}/gates.v -top dut -lib ${P}/pw.lib -clock nope 10 RESULT_VARIABLE rc ERROR_VARIABLE err OUTPUT_QUIET)
if(rc EQUAL 0 OR NOT err MATCHES "no net 'nope'")
  message(FATAL_ERROR "bad -clock: rc ${rc}: ${err}")
endif()
