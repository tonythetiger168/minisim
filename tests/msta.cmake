# msta self-test on tests/sta: the numbers below are worked out by hand from
# the linear tables of tests/sta/sta_test.lib (see the comment in that file).
#   cmake -DMSTA=<msta> -DSRC=<source dir> -DOUT=<scratch dir> [-DPYTHON=<python3>] -P msta.cmake
set(LIB ${SRC}/tests/sta/sta_test.lib)
function(run rcwant)
  execute_process(COMMAND ${MSTA} ${ARGN} WORKING_DIRECTORY ${SRC}/tests/sta
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  set(out "${out}" PARENT_SCOPE)
  set(err "${err}" PARENT_SCOPE)
  if(NOT rc EQUAL ${rcwant})
    message(FATAL_ERROR "msta ${ARGN} exited ${rc}, expected ${rcwant}\n${out}${err}")
  endif()
endfunction()
function(want)
  foreach(w ${ARGN})
    string(FIND "${out}" "${w}" pos)
    if(pos EQUAL -1)
      message(FATAL_ERROR "missing: ${w}\n${out}${err}")
    endif()
  endforeach()
endfunction()
file(MAKE_DIRECTORY ${OUT})

# --- pipe: two registers, ideal clock, period 2.
#  max: r2/CK -> r2/Q drives BUF A (0.002 pF): 0.15 + 2*0.002 = 0.154, slew 0.026.
#       u3 into the 0.01 pF port load: 0.08 + 0.2*0.026 + 2*0.01 = 0.1052.
#       arrival 0.259, required 2 - 0.4 = 1.6, slack 1.341.
#  min: r1/Q rise 0.158, u2 B->Y fall 0.073, hold 0.029: slack 0.202.
run(0 -lib ${LIB} pipe.v -sdc pipe.sdc)
want("check_timing: no problems found."
     "Startpoint: r2 (rising edge-triggered flip-flop clocked by clk)"
     "Endpoint: out1 (output port clocked by clk)"
     "r2/Q (DFF_X1)                                 0.154      0.154 r"
     "u3/Y (BUF_X1)                                 0.105      0.259 r"
     "output external delay                        -0.400      1.600"
     "slack (MET)                                              1.341"
     "library hold time                             0.029      0.029"
     "slack (MET)                                              0.202"
     "clk                   max            1.341       0.000           0           3"
     "clk                   min            0.202       0.000           0           3"
     "All constraints are met.")

# A period too short for the output: violated, exit 2 with -fail_on_violation.
run(2 -lib ${LIB} pipe.v -sdc pipe.sdc -e "create_clock -name clk -period 0.6 [get_ports clk]\; report_timing" -fail_on_violation)
want("slack (VIOLATED)                                        -0.059")

# --- soc: propagated clock through a buffer, a falling-edge register, a
# second clock, a divider with a generated clock, recovery and removal, a
# multicycle path and a false path.
#  clock buffer into five clock pins (0.01 pF): rise 0.08 + 0.02 = 0.100, fall 0.090.
#  ra1/Q: 0.15 + 0.2*0.05 + 2*0.005 = 0.170; x1 B->Y: 0.13 + 0.2*0.04 + 2*0.004 = 0.146.
#  rn/D (falling edge): required 2 + 0.090 - 0.1 - 0.062 = 1.928.
#  rc/D: clk falls at 6, clk2 rises at 7: the closest pair of edges.
#  rd/CK: the generated clock's latency is the master's arrival at div/Q, 0.268.
run(0 -lib ${LIB} soc.v -f soc.tcl)
want("check_timing: 0"
     "clkdiv                   8.000   {0.000 4.000}       G     {div/Q}"
     "rn/D (DFFN_X1)          1.928       0.416       1.512"
     "rc/D (DFF_X1)           6.944       6.253       0.691"
     "rd/D (DFF_X1)           8.211       4.396       3.815"
     "rb/D (DFFR_X1)          4.182       0.495      -3.687  (VIOLATED)"
     "ra1/Q (DFF_X1)                                0.005      0.040      0.170      0.270 r"
     "x1/B (XOR2_X1)                                           0.040      0.000      0.270 r"
     "clock clk (fall edge)                         6.000      6.000"
     "clock clk2 (rise edge)                        7.000      7.000"
     "Endpoint: rb (recovery check against rising-edge clock clk)"
     "library recovery time                        -0.045      3.955"
     "library removal time                          0.033      0.182"
     "clk                   min           -3.687      -3.687           1           7"
     "Levels of Logic:                 1"
     "Sequential Cells:                7"
     "wns 0.6909 tns 0")
if(out MATCHES "sel \\(in\\)")
  message(FATAL_ERROR "the false path from sel was reported\n${out}")
endif()
#  Hold at rc/D, between clocks of different periods (clk 4, clk2 6, common
#  period 12). clk falls at 2, 6, 10; clk2 rises at 1, 7, 13. The setup pairs
#  are 6 -> 7 and 10 -> 13, so the hold pairs are 6 -> 1, 10 -> 7 (twice) and
#  14 -> 13: the last is the tightest, the same as 2 -> 1.
#  Arrival 2 + 0.090 + 0.148 = 2.238, required 1 + 0.029: slack 1.208.
want("clock clk (fall edge)                         2.000      2.000"
     "clock clk2 (rise edge)                        1.000      1.000"
     "slack (MET)                                              1.208")

# An asynchronous reset to output arc is not a path unless asked for.
#  rst_n arrives at 0.5, RN -> Q 0.236; required 4 - 0.1 - 1.5: slack 1.664.
run(0 -lib ${LIB} soc.v -f soc.tcl -e "report_timing -from [get_ports rst_n] -to [get_ports {y[0]}]")
want("No constrained max paths.")
run(0 -lib ${LIB} soc.v -f soc.tcl -preset_clear_arcs -e "report_timing -from [get_ports rst_n] -to [get_ports {y[0]}]")
want("rb/Q (DFFR_X1)                                0.236      0.736 f"
     "slack (MET)                                              1.664")

# --- on-chip variation: set_timing_derate and clock reconvergence pessimism.
#  clk -> cb0 -> cb1 -> r1, r2, rn (falling edge);  cb0 -> cb2 -> r3.
#  Early 0.9, late 1.2. cb0 rises in 0.088 and falls in 0.0792; cb1 rises in 0.0984.
#  r1 -> r2 share cb0 and cb1: (0.088 + 0.0984) * (1.2 - 0.9) = 0.056 is removed.
#  rn -> r3 share cb0 only, falling for the launch and rising for the capture:
#  the smaller of 0.0792 * 0.3 and 0.088 * 0.3 = 0.024.
#  The same numbers come out of OpenSTA.
run(0 -lib ${LIB} ocv.v -sdc ocv.sdc -e "report_timing -max_paths 20 -path end\; report_timing -delay_type min -max_paths 20 -path end\; report_timing -to [get_pins r3/D] -path full_clock\; report_timing -delay_type min -to [get_pins r2/D]")
want("r3/D (DFF_X1)          4.126       2.539       1.586"
     "r2/D (DFF_X1)          4.163       0.649       3.515"
     "r2/D (DFF_X1)          0.199       0.424       0.225"
     "rn/D (DFFN_X1)        -1.818       0.308       2.126"
     "clock network delay (propagated)              0.202      2.202"
     "clock network delay (propagated)              0.161      4.161"
     "clock reconvergence pessimism                 0.024      4.184"
     "clock reconvergence pessimism                -0.056      0.168")
#  Without the removal r2/D loses the 0.056; with -crpr_same_transition only
#  the rn -> r3 path loses its 0.024.
run(0 -lib ${LIB} ocv.v -sdc ocv.sdc -no_crpr -e "report_timing -max_paths 20 -path end")
want("r3/D (DFF_X1)          4.102       2.539       1.562"
     "r2/D (DFF_X1)          4.108       0.649       3.459")
run(0 -lib ${LIB} ocv.v -sdc ocv.sdc -crpr_same_transition -e "report_timing -max_paths 20 -path end")
want("r3/D (DFF_X1)          4.102       2.539       1.562"
     "r2/D (DFF_X1)          4.163       0.649       3.515")
#  Factors on one instance, on a library cell and on the timing checks
#  (again the same as OpenSTA): cb1 late 1.5 makes the launch clock 0.253 and
#  the removed pessimism 0.085; XOR2 early 0.8 on data; setup time * 1.1 and
#  hold time * 1.3.
run(0 -lib ${LIB} ocv.v -sdc ocv2.sdc -e "report_timing -max_paths 20 -path end\; report_timing -delay_type min -max_paths 20 -path end\; report_timing -delay_type min -to [get_pins r2/D]")
want("r3/D (DFF_X1)          4.120       2.566       1.554"
     "r2/D (DFF_X1)          4.187       0.678       3.509"
     "u4/Y (XOR2_X1)                                0.103      0.411 f"
     "clock reconvergence pessimism                -0.085      0.168"
     "library hold time                             0.041      0.209"
     "slack (MET)                                              0.202")

# --- clock gating checks, minimum pulse width and minimum period.
#  clk (period 2, high for 0.8) -> cb: rises in 0.104, falls in 0.0936.
#  g1 (AND, enable from the falling-edge rn): setup to the rising edge,
#    2 + 0.104 - 0.1 = 2.004 against 0.8 + 0.0936 + 0.164 = 1.058; hold to the
#    falling edge, 0.8 + 0.0936 + 0.05 = 0.944 against 1.042.
#  g3 (AND, enable from the rising-edge re): the enable changes at 0.257
#    while the clock is still high until 0.944: a hold violation of 0.686.
#  g2 (OR): setup to the falling edge 0.8936 - 0.1, hold to the rising edge
#    0.104 + 0.05 = 0.154 against 0.257.
#  rf/CK is behind an inverter: its low pulse opens at 0.164 and closes at
#    0.958, 0.794 wide against the library's 0.27 (a min_pulse_width group);
#    DFF_X1 has min_pulse_width_high / _low / min_period attributes.
run(0 -lib ${LIB} gate.v -sdc gate.sdc -e "report_timing -max_paths 20 -path end\; report_timing -delay_type min -max_paths 20 -path end\; report_timing -delay_type min -to [get_pins g3/B] -path full_clock\; report_min_pulse_width\; report_min_pulse_width -verbose -max_paths 1\; report_min_period\; report_constraint")
want("g2/B (OR2_X1)          0.794       0.273       0.520"
     "g1/B (AND2_X1)         2.004       1.058       0.946"
     "g3/B (AND2_X1)         2.004       0.273       1.731"
     "g3/B (AND2_X1)         0.944       0.257      -0.686  (VIOLATED)"
     "g1/B (AND2_X1)         0.944       1.042       0.099"
     "g2/B (OR2_X1)          0.154       0.257       0.103"
     "Endpoint: g3 (rising clock gating-check end-point clocked by clk)"
     "g3/A (AND2_X1)                                0.000      0.894 f"
     "clock gating hold time                        0.050      0.944"
     "rf/CK (low)          0.270       0.794       0.524"
     "r2/CK (high)         0.250       0.777       0.527"
     "rn/CK (high)         0.220       0.790       0.570"
     "r2/CK (low)          0.200       1.223       1.023"
     "Check: min_pulse_width (low)"
     "open edge arrival time                                   0.164"
     "close edge arrival time                                  0.958"
     "actual pulse width                                       0.794"
     "rn/CK            0.600       2.000       1.400"
     "re/CK            0.500       2.000       1.500"
     "clk                   min           -0.686      -0.686           1          13")
#  set_min_pulse_width on a clock (register clock pins) and on a pin; a
#  period below the library's minimum; no gating checks when turned off.
run(2 -lib ${LIB} gate.v -sdc gate.sdc -no_clock_gating_checks -fail_on_violation -e "set_min_pulse_width -high 0.78 [get_clocks clk]\; set_min_pulse_width -low 1.3 [get_pins rn/CK]\; report_constraint -all_violators")
want("clk                   min            0.039       0.000           0          10"
     "min_pulse_width: 4 violations"
     "rn/CK (low)          1.300       1.210      -0.090  (VIOLATED)"
     "r2/CK (high)         0.780       0.777      -0.003  (VIOLATED)"
     "r1/CK (high)         0.780       0.778      -0.002  (VIOLATED)")
run(2 -lib ${LIB} gate.v -sdc gate.sdc -no_clock_gating_checks -fail_on_violation -e "create_clock -name clk -period 0.55 [get_ports clk]\; report_constraint -all_violators")
want("min_period: 2 violations"
     "rn/CK            0.600       0.550      -0.050  (VIOLATED)")

# --- exceptions and the rest of the constraints.
#  clock groups cut clk2 from clk; latency 0.3 + 0.2 source on the ideal clk;
#  the false path through x1/Y then m1/Y leaves sel as rb/D's worst path;
#  multicycle 3 moves rn/D's capture edge to 10, and hold 2 its launch to 8.
run(0 -lib ${LIB} soc.v -f exc.tcl)
want("Warning: 1 register clock pins with no clock (no_clock)"
     "    rd/CK"
     "Warning: 6 endpoints that are not constrained (unconstrained_endpoints)"
     "Warning: 1 input ports with no input delay (no_input_delay)"
     "rn/D (DFFN_X1)        10.443       0.805       9.638"
     "clock network delay (ideal)                   0.500      0.500"
     "clock clk (fall edge)                        10.000     10.000"
     "clock clk (rise edge)                         8.000      8.000"
     "Startpoint: sel (input port clocked by vclk)"
     "sel (in)                                      0.004      1.004 r"
     "max_transition: 15 violations"
     "x1/Y             0.030       0.036      -0.006  (VIOLATED)"
     "max_capacitance: 1 violation"
     "ra1/Q            0.004       0.005      -0.001  (VIOLATED)")

# --- constants: with sel = 0 the mux no longer passes its B input (the slow
# path through two buffers), and with en = 1 the XOR only inverts, so a rise
# of ra/Q arrives at x1/Y as a fall. The tied AND input is never a path.
run(0 -lib ${LIB} case.v -f case.tcl)
want("rc/D (DFF_X1)         1.944       0.592       1.352"
     "x1/Y (XOR2_X1)                                0.130      0.286 r"
     "slack (MET)                                              1.409"
     "rc/D (DFF_X1)         1.944       0.519       1.425")

# --- SPEF: net q1 has 6 fF of wire, so r1/Q drives 0.010 pF: 0.15 + 2*0.010 = 0.170.
# Elmore delay to u1/A: 100 ohm * (2 + 3 + 4 fF behind it) + 50 ohm * 3 fF = 1.05 fs*1000.
run(0 -lib ${LIB} pipe.v -sdc pipe.sdc -spef pipe.spef
    -e "report_timing -to [get_pins r2/D] -input_pins -capacitance -transition -significant_digits 4")
want("read_spef: 2 of 7 nets annotated"
     "r1/Q (DFF_X1)                                 0.0100      0.0500      0.1700      0.1700 r"
     "u1/A (INV_X1)                                             0.0501      0.0010      0.1710 r"
     "slack (MET)                                                                       1.6338")

# --- SDF and JSON.
run(0 -lib ${LIB} soc.v -q -e "create_clock -name clk -period 4 [get_ports clk]" -sdf ${OUT}/soc.sdf -json ${OUT}/soc.json)
file(READ ${OUT}/soc.sdf out)
want("(DESIGN \"soc\")" "(TIMESCALE 1ns)" "(CELLTYPE \"DFFN_X1\")" "(INSTANCE rn)"
     "(IOPATH (negedge CK) Q" "(IOPATH A Y (0.1000::0.1000) (0.0900::0.0900))"
     "(SETUP D (negedge CK)" "(RECOVERY RN (posedge CK)" "(REMOVAL RN (posedge CK)")
file(READ ${OUT}/soc.json out)
want("\"design\": \"soc\"" "\"group\": \"clk\", \"type\": \"max\"" "\"endpoint\": \"rn/D\"")

# --- errors and messages.
run(1 -lib ${LIB} soc.v -top nope)
if(NOT err MATCHES "no module named nope")
  message(FATAL_ERROR "missing 'no module named nope'\n${err}")
endif()
run(1 -lib ${LIB} soc.v -e "report_timing -bogus")
if(NOT err MATCHES "report_timing: unknown option -bogus")
  message(FATAL_ERROR "missing the unknown option error\n${err}")
endif()
file(WRITE ${OUT}/loop.v "module loop (input a, output y);\n wire n1, n2;\n NAND2_X1 g1 (.A(a), .B(n2), .Y(n1));\n INV_X1 g2 (.A(n1), .Y(n2));\n BUF_X1 g3 (.A(n1), .Y(y));\n FOO_X1 g4 (.A(n1));\nendmodule\n")
run(0 -lib ${LIB} ${OUT}/loop.v -e "check_timing")
want("combinational loops, cut at (loops)" "cell FOO_X1 is not in any library")

# --- the same answer with any number of threads, on a few thousand cells.
if(PYTHON)
  execute_process(COMMAND ${PYTHON} ${SRC}/tests/sta/gen_netlist.py 20000 ${OUT}/big.v ${OUT}/big.sdc 7 RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "gen_netlist.py failed")
  endif()
  foreach(t 1 2 8)
    run(0 -lib ${LIB} ${OUT}/big.v -sdc ${OUT}/big.sdc -threads ${t} -q -json ${OUT}/big${t}.json -sdf ${OUT}/big${t}.sdf
        -e "report_timing -max_paths 5\; report_timing -delay_type min -max_paths 5\; report_constraint -all_violators")
    set(rep${t} "${out}")
    file(SHA256 ${OUT}/big${t}.json j${t})
    file(SHA256 ${OUT}/big${t}.sdf s${t})
  endforeach()
  if(NOT rep1 STREQUAL rep2 OR NOT rep1 STREQUAL rep8 OR NOT j1 STREQUAL j2 OR NOT j1 STREQUAL j8
     OR NOT s1 STREQUAL s2 OR NOT s1 STREQUAL s8)
    message(FATAL_ERROR "msta's results depend on the number of threads")
  endif()
  if(NOT rep1 MATCHES "Path Group: clk2" OR NOT rep1 MATCHES "slack \\(")
    message(FATAL_ERROR "the big design reported no paths\n${rep1}")
  endif()
endif()

# --- the mini45 library (lib/mini45), three corners on the pipeline. No
# numbers are pinned here, because the library's tables are regenerated by
# characterization; what must hold is the order of the corners: the slow
# corner has the least setup slack and the most hold slack.
set(M45 ${SRC}/lib/mini45)
if(EXISTS ${M45}/mini45_tt.lib)
  foreach(c ss tt ff)
    run(0 -lib ${M45}/mini45_${c}.lib pipe.v -sdc pipe.sdc -json ${OUT}/m45_${c}.json)
    want("check_timing: no problems found." "All constraints are met.")
    file(READ ${OUT}/m45_${c}.json j)
    if(NOT j MATCHES "\"type\": \"max\", \"wns\": ([-0-9.]+)")
      message(FATAL_ERROR "mini45 ${c}: no setup summary\n${j}")
    endif()
    set(max_${c} ${CMAKE_MATCH_1})
    if(NOT j MATCHES "\"type\": \"min\", \"wns\": ([-0-9.]+)")
      message(FATAL_ERROR "mini45 ${c}: no hold summary\n${j}")
    endif()
    set(min_${c} ${CMAKE_MATCH_1})
  endforeach()
  if(NOT (max_ss LESS max_tt AND max_tt LESS max_ff AND min_ss GREATER min_tt AND min_tt GREATER min_ff))
    message(FATAL_ERROR "mini45 corners out of order: setup ss ${max_ss} tt ${max_tt} ff ${max_ff}, "
                        "hold ss ${min_ss} tt ${min_tt} ff ${min_ff}")
  endif()
endif()
