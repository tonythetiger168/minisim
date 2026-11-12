# Power-aware simulation (msim -upf) and UPF checks (mlint -upf), IEEE 1801-2024.
#   cmake -DMSIM=<msim> -DMLINT=<mlint> -DSRC=<source dir> -P upf.cmake
# Each tests/upf/<name>.sv runs with <name>.upf and must print <name>.out.
set(D ${SRC}/tests/upf)
foreach(t gating retention states vct)
  execute_process(COMMAND ${MSIM} ${D}/${t}.sv -upf ${D}/${t}.upf
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "msim ${t}.sv -upf ${t}.upf exited ${rc}\n${out}${err}")
  endif()
  file(READ ${D}/${t}.out want)
  if(NOT out STREQUAL want)
    message(FATAL_ERROR "${t}: output differs\n--- got\n${out}--- want\n${want}")
  endif()
endforeach()

# Logic nets and ports made by the power intent, repeaters (6.13, 6.19, 6.20,
# 6.52, 9.10); named state transitions and power state groups, which report
# through -upf_verbose (6.7, 6.22, 9.4).
foreach(t logic transitions)
  set(flags)
  if(t STREQUAL "transitions")
    set(flags -upf_verbose)
  endif()
  execute_process(COMMAND ${MSIM} ${D}/${t}.sv -upf ${D}/${t}.upf ${flags}
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  file(READ ${D}/${t}.out want)
  if(NOT rc EQUAL 0 OR NOT out STREQUAL want)
    message(FATAL_ERROR "${t}: ${rc}\n--- got\n${out}${err}--- want\n${want}")
  endif()
endforeach()
# Errors of the logic and transition commands, each with its file and line.
function(upf_error name text want)
  file(READ ${D}/logic.upf base)
  file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/${name}.upf "${base}${text}\n")
  execute_process(COMMAND ${MSIM} ${D}/logic.sv -upf ${CMAKE_CURRENT_BINARY_DIR}/${name}.upf
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  string(FIND "${err}" "${want}" pos)
  if(rc EQUAL 0 OR pos EQUAL -1)
    message(FATAL_ERROR "${name}: expected the error '${want}': ${rc}\n${out}${err}")
  endif()
endfunction()
upf_error(e_const "create_logic_net t1\nconnect_logic_net t1 -ports {u_core/d}" "must both be one bit of logic (6.13)")
upf_error(e_net "connect_logic_net nope -ports {u_core/r}" "no logic net 'u_dut.nope'")
upf_error(e_two "create_logic_net t2\nconnect_logic_net t2 -ports {u_pmu/iso}" "is already connected to logic net iso_w (6.13 e)")
upf_error(e_drv "create_logic_net t3\nconnect_logic_net t3 -ports {u_pmu/req}" "is already connected to another net (6.13 b)")
upf_error(e_rep "set_repeater R2 -domain PD_CORE" "set_repeater needs -repeater_supply (6.52)")
upf_error(e_tr1 "add_state_transition PD_CORE -domain -transition {t -from A}" "needs both -from and -to (6.7)")
upf_error(e_tr2 "add_power_state PD_CORE -domain -state {A -logic_expr {u_pmu/req}}\nadd_state_transition PD_CORE -domain -transition {t -from A -to B}" "B is not a power state of PD_CORE (6.7)")
upf_error(e_tr3 "add_state_transition PD_CORE -domain -transition {t -from A -to A}" "A is both the -from and the -to state (6.7)")
# UPF is Tcl (5.1): the same intent written with proc, foreach, if, expr and
# [command] substitution simulates the same.
execute_process(COMMAND ${MSIM} ${D}/gating.sv -upf ${D}/tcl.upf
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
file(READ ${D}/gating.out want)
if(NOT rc EQUAL 0 OR NOT out STREQUAL want)
  message(FATAL_ERROR "tcl.upf: ${rc}\n${out}${err}")
endif()
# An error inside a proc called from a loop is reported where it is.
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/badtcl.upf "proc p {d} {\n  create_power_domain $d -update\n}\nforeach d {A B} { p $d }\n")
execute_process(COMMAND ${MSIM} ${D}/gating.sv -upf ${CMAKE_CURRENT_BINARY_DIR}/badtcl.upf
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(rc EQUAL 0 OR NOT err MATCHES "badtcl.upf:2: -update of unknown domain A")
  message(FATAL_ERROR "an error in a UPF proc was not reported: ${rc}\n${out}${err}")
endif()

# Value conversion tables (4.10): a table against the port's direction, a
# port that needs -vct, and an unknown table are errors.
foreach(case
    "connect_supply_net VREG -ports {u_mac/VDD} -vct SV_LOGIC2UPF|u_dut.u_mac.VDD is an input port, but SV_LOGIC2UPF converts an HDL value to a supply state"
    "connect_supply_net VREG -ports {vreg}|u_dut.vreg is not an input or output port of an instance; name a value conversion table with -vct"
    "connect_supply_net VREG -ports {u_mac/a} -vct UPF2SV_LOGIC|u_dut.u_mac.a is 4 bits wide"
    "connect_supply_net VREG -ports {u_mac/VDD} -vct NOPE|no value conversion table 'NOPE'"
    "create_hdl2upf_vct T -hdl_type {sv} -table {{2 OFF}}|'2' is not a value of SystemVerilog logic"
    "create_upf2hdl_vct UPF2SV_LOGIC -hdl_type {sv} -table {{OFF 0}}|UPF2SV_LOGIC is already defined [(]Annex B[)]")
  string(REPLACE "|" ";" case "${case}")
  list(GET case 0 line)
  list(GET case 1 want)
  file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/badvct.upf "set_design_top dut\ncreate_power_domain PD_TOP -include_scope\ncreate_supply_net VREG\n${line}\n")
  execute_process(COMMAND ${MSIM} ${D}/vct.sv -upf ${CMAKE_CURRENT_BINARY_DIR}/badvct.upf
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(rc EQUAL 0 OR NOT err MATCHES "${want}")
    message(FATAL_ERROR "VCT error not reported for: ${line}\nwant: ${want}\n${rc}\n${out}${err}")
  endif()
endforeach()

# -power_top names the instance the UPF describes; it wins over set_design_top.
execute_process(COMMAND ${MSIM} ${D}/gating.sv -upf ${D}/gating.upf -power_top tb/u_dut
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
file(READ ${D}/gating.out want)
if(NOT rc EQUAL 0 OR NOT out STREQUAL want)
  message(FATAL_ERROR "-power_top tb/u_dut: ${rc}\n${out}${err}")
endif()

# A UPF error stops before simulation, with the file and line.
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/bad.upf "create_power_domain PD -elements {u_core}\nset_isolatoin X -domain PD\n")
execute_process(COMMAND ${MSIM} ${D}/gating.sv -upf ${CMAKE_CURRENT_BINARY_DIR}/bad.upf
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(rc EQUAL 0 OR NOT err MATCHES "bad.upf:2: unknown UPF command set_isolatoin")
  message(FATAL_ERROR "a bad UPF file was not reported: ${rc}\n${out}${err}")
endif()

# mlint: the good intent is clean; the bad one reports every rule.
execute_process(COMMAND ${MLINT} ${D}/gating.sv -upf ${D}/gating.upf -only upf
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT out MATCHES "0 findings")
  message(FATAL_ERROR "mlint gating.upf is not clean: ${rc}\n${out}${err}")
endif()
execute_process(COMMAND ${MLINT} ${D}/gating.sv -upf ${D}/lint_bad.upf -only upf
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
foreach(want
    "[UPF001] power domain PD_GONE: no instance 'u_dut.u_gone'"
    "[UPF001] isolation ISO_IN: no signal 'u_dut.no_such_signal'"
    "[UPF002] power domain PD_GONE: primary supply PD_GONE.primary has no power function"
    "[UPF003] no isolation on u_dut.u_core.q (PD_CORE -> PD_TOP)"
    "[UPF004] no level shifter on u_dut.u_core.d (PD_TOP -> PD_CORE)"
    "[UPF005] isolation on u_dut.u_core.clk (PD_TOP -> PD_CORE)"
    "[UPF006] PST P state odd: VDDL has no port state MID"
    "18 findings")
  string(FIND "${out}" "${want}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "mlint lint_bad.upf: missing: ${want}\n${out}${err}")
  endif()
endforeach()
