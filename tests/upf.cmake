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
# through -upf_verbose (6.7, 6.22, 9.4); attributes of ports and models,
# equivalent supplies, composite domains, explicit supply connections and
# simstate behavior (5.6, 6.8, 6.15, 6.17, 6.44, 6.47, 6.50, 6.51, 6.56);
# corruption and assertion controls and a library's simstate behavior file
# (6.34, 6.58, 6.59); power models, applied by name and to every instance of
# a model, with a parameter, and a legacy one (6.3, 6.9, 6.11, 6.31, 6.32).
foreach(t logic transitions attributes simctl pmodel)
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
function(upf_error_in t name text want)
  file(READ ${D}/${t}.upf base)
  file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/${name}.upf "${base}${text}\n")
  execute_process(COMMAND ${MSIM} ${D}/${t}.sv -upf ${CMAKE_CURRENT_BINARY_DIR}/${name}.upf
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  string(FIND "${err}" "${want}" pos)
  if(rc EQUAL 0 OR pos EQUAL -1)
    message(FATAL_ERROR "${name}: expected the error '${want}': ${rc}\n${out}${err}")
  endif()
endfunction()
function(upf_error name text want)
  upf_error_in(logic ${name} "${text}" "${want}")
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

# Errors of the attribute, equivalence and composite domain commands.
set(comp "create_supply_set SS_X -function {power VDD_E} -function {ground VSS}\ncreate_power_domain PD_X -elements {u_e}\nassociate_supply_set SS_X -handle PD_X.primary\ncreate_composite_domain PD_C -subdomains {PD_A PD_X} -supply {primary SS_A}")
upf_error_in(attributes e_comp "${comp}" "the primary supply of subdomain PD_X (SS_X) is not equivalent to SS_A (6.17 c)")
upf_error_in(attributes e_pco "set_simstate_behavior PORT_CORR_ONLY -elements {u_a}" "which is not a hard macro (UPF_is_hard_macro, 6.56)")
upf_error_in(attributes e_eq "set_equivalent -nets {VDD}" "set_equivalent needs two or more supplies")
upf_error_in(attributes e_eq2 "set_equivalent -nets {VDD VSS} -sets {SS SS_A}" "needs -nets or -sets, not both (6.47)")
upf_error_in(attributes e_part "set_partial_on_translation FULL_ON" "set_partial_on_translation FULL_ON after OFF (6.50)")
upf_error_in(attributes e_drv "set_port_attributes -ports {u_a/q} -driver_supply SS_E" "the logic that drives it is on SS_A (6.51)")
upf_error_in(attributes e_attr "set_design_attributes -models {ip} -attribute {UPF_retention maybe}" "attribute UPF_retention cannot be maybe (5.6)")
upf_error_in(attributes e_sst "add_supply_state NOPE -state {ON 1.0}" "no supply port, net or supply set function NOPE")
upf_error_in(attributes e_css "connect_supply_set SS_E" "connect_supply_set needs -elements")
# a retention list (6.54): a strategy can name it; a required element that no
# strategy of its domain retains is an error
file(READ ${D}/retention.upf base)
string(REPLACE "set_retention RET_A -domain PD_A -elements {u_a/keep}"
       "set_retention_elements KEEP_A -elements {u_a/keep}\nset_retention RET_A -domain PD_A -elements {KEEP_A}" withlist "${base}")
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/retlist.upf "${withlist}")
execute_process(COMMAND ${MSIM} ${D}/retention.sv -upf ${CMAKE_CURRENT_BINARY_DIR}/retlist.upf
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
file(READ ${D}/retention.out want)
if(NOT rc EQUAL 0 OR NOT out STREQUAL want)
  message(FATAL_ERROR "set_retention_elements: ${rc}\n${out}${err}")
endif()
upf_error_in(retention e_retl "set_retention_elements L -elements {u_a/lose}" "retention list L: u_a.lose is not retained, and other elements of PD_A are (6.54)")
# mlint reports the same intent errors as UPF007
file(READ ${D}/attributes.upf base)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/l_comp.upf "${base}${comp}\n")
execute_process(COMMAND ${MLINT} ${D}/attributes.sv -upf ${CMAKE_CURRENT_BINARY_DIR}/l_comp.upf
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
string(FIND "${out}${err}" "[UPF007] composite domain PD_C: the primary supply of subdomain PD_X" pos)
if(pos EQUAL -1)
  message(FATAL_ERROR "mlint: UPF007 was not reported\n${out}${err}")
endif()

# Errors of the simulation controls and load_simstate_behavior. The copies
# of simctl.upf read simctl_lib.upf next to them.
file(COPY ${D}/simctl_lib.upf DESTINATION ${CMAKE_CURRENT_BINARY_DIR})
upf_error_in(simctl e_ac1 "sim_assertion_control -elements {u_k} -controlling_domain PD_SW -control_expr {quiet}" "-controlling_domain and -control_expr together (6.58)")
upf_error_in(simctl e_ac2 "sim_assertion_control -elements {u_k} -type pause" "sim_assertion_control -type pause (reset, suspend or kill)")
upf_error_in(simctl e_ac3 "sim_assertion_control -domain PD_NONE" "sim_assertion_control: no power domain PD_NONE")
upf_error_in(simctl e_cc1 "sim_corruption_control -elements {u_c}" "sim_corruption_control needs -type")
upf_error_in(simctl e_cc2 "sim_corruption_control -type float" "sim_corruption_control -type float (real, integer, seq, comb, port or process)")
upf_error_in(simctl e_cc3 "sim_corruption_control -type real -control_expr {quiet}" "sim_corruption_control: unknown option -control_expr")
upf_error_in(simctl e_ls1 "load_simstate_behavior mylib -file nope.upf" "load_simstate_behavior: no file")
foreach(case
    "set_simstate_behavior DISABLE -models {beh} -lib x|set_simstate_behavior -lib in a simstate behavior file (6.34)"
    "create_power_domain PD_Q -elements {u_m}|create_power_domain in a simstate behavior file; only set_simstate_behavior is allowed (6.34)"
    "set_simstate_behavior DISABLE -models {nosuch}|load_simstate_behavior: model nosuch is not in the design (6.34)")
  string(REPLACE "|" ";" case "${case}")
  list(GET case 0 line)
  list(GET case 1 want)
  file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/badlib.upf "${line}\n")
  upf_error_in(simctl e_lib "load_simstate_behavior mylib -file ${CMAKE_CURRENT_BINARY_DIR}/badlib.upf" "${want}")
endforeach()

# Errors of power models and parameters.
upf_error_in(pmodel e_pm1 "define_power_model MAC_PM {}" "power model MAC_PM is already defined (6.31)")
upf_error_in(pmodel e_pm2 "define_power_model P2 -complete {}\ndefine_power_model P2 -update {}" "power model P2 is -complete; it cannot be updated (6.31)")
upf_error_in(pmodel e_pm3 "define_power_model P3 {define_power_model P4 {}}" "define_power_model inside power model P3; power models do not nest (6.31)")
upf_error_in(pmodel e_pm4 "apply_power_model NOPE -elements {u_mac0}" "apply_power_model: no power model NOPE (6.9)")
upf_error_in(pmodel e_pm5 "define_power_model P5 {add_parameter X -default 1}\napply_power_model P5 -elements {u_mac0/u_core} -parameters {{Y 2}}" "Y is not a parameter of power model P5 (6.9)")
upf_error_in(pmodel e_pm6 "define_power_model P6 {add_parameter X}\napply_power_model P6 -elements {u_mac0/u_core}" "add_parameter X needs -default (6.3 b)")
upf_error_in(pmodel e_pm7 "define_power_model P7 {\n  add_parameter X -default 1\n  add_parameter X -default 2\n}\napply_power_model P7 -elements {u_mac0/u_core}" "e_pm7.upf:48: parameter X is already defined in power model P7 (6.3 a)")
upf_error_in(pmodel e_pm8 "begin_power_model P8" "begin_power_model P8 has no end_power_model (6.11)")
upf_error_in(pmodel e_pm9 "end_power_model" "end_power_model without begin_power_model (6.32)")
upf_error_in(pmodel e_pm10 "apply_power_model ROM_PM -elements {u_rom}" "power model ROM_PM is already applied to u_rom")
upf_error_in(pmodel e_pm11 "define_power_model P9 {}\napply_power_model P9 -elements {u_mac0} -update" "apply_power_model -update: P9 is not applied to u_mac0 (6.9)")
upf_error_in(pmodel e_pm12 "define_power_model P5 {}\napply_power_model P5 -elements {u_rom/x}" "apply_power_model P5: no instance 'u_dut.u_rom.x' in the design (6.9)")
upf_error_in(pmodel e_pm13 "define_power_model P10 -for {rom} {}\napply_power_model P10 -elements {u_mac0/u_core}" "an instance of mac_core, which is not a model it is for (6.31)")
upf_error_in(pmodel e_pm14 "add_parameter Q -type sometimes -default 1" "add_parameter -type sometimes (buildtime, runtime or rate)")

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
