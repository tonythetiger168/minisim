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
# a model, with a parameter, and a legacy one (6.3, 6.9, 6.11, 6.31, 6.32);
# checkers bound into the design by the power intent (6.12); initial blocks
# replayed at power-up (6.60); attributes written in the HDL (5.6, 5.7); an
# abstract power source (6.16); value conversion methods in UPF libraries,
# for real, enum, integer and upfSupplyTypeT ports (4.10, 5.11, 6.29, 6.30,
# 6.36, 6.63); function and model VCMs (6.30.8, 6.30.9); supply ports
# connected by pg_type (4.5.4.6.2, 6.14, 6.15).
foreach(t logic transitions attributes simctl pmodel bindchk replay hdlattr source vcm vcmfn pgconn)
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

# Errors of bind_checker; a parameter the checker does not have is reported
# by the elaboration of the bind directive.
upf_error_in(bindchk e_bc1 "bind_checker c2 -bind_to leaf" "bind_checker needs -module")
upf_error_in(bindchk e_bc2 "bind_checker c3 -module pwr_chk -arch rtl -elements {u_core}" "bind_checker -arch without -bind_to")
upf_error_in(bindchk e_bc3 "bind_checker chk_leaf -module pwr_chk -bind_to leaf" "bind_checker: chk_leaf already exists in leaf (6.12)")
upf_error_in(bindchk e_bc4 "bind_checker c4 -module pwr_chk -parameters {{NOPE 1}} -ports {{clk clk} {v q} {en on}}" "parameter 'NOPE' does not exist in 'pwr_chk'")

# Errors of sim_replay_control and save_upf.
upf_error_in(replay e_rp1 "sim_replay_control" "sim_replay_control needs -elements, -model or -domain (6.60)")
upf_error_in(replay e_rp2 "sim_replay_control -elements {u_rom}" "u_dut.u_rom is already covered by sim_replay_control -model rom")
upf_error_in(replay e_rp3 "sim_replay_control -domain NOPE" "sim_replay_control: no power domain NOPE")
upf_error_in(replay e_sv1 "save_upf a.upf\nsave_upf a.upf" "save_upf a.upf is already written by")

# save_upf (6.42): the file it writes is the same power intent; the
# simulation with it prints the same.
foreach(t pmodel attributes simctl)
  file(READ ${D}/${t}.upf base)
  file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/save_${t}.upf "${base}save_upf saved_${t}.upf\n")
  execute_process(COMMAND ${MSIM} ${D}/${t}.sv -upf ${CMAKE_CURRENT_BINARY_DIR}/save_${t}.upf
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  execute_process(COMMAND ${MSIM} ${D}/${t}.sv -upf ${CMAKE_CURRENT_BINARY_DIR}/saved_${t}.upf
                  RESULT_VARIABLE rc2 OUTPUT_VARIABLE out2 ERROR_VARIABLE err2)
  file(READ ${D}/${t}.out want)
  if(NOT rc EQUAL 0 OR NOT rc2 EQUAL 0 OR NOT out STREQUAL want OR NOT out2 STREQUAL want)
    message(FATAL_ERROR "save_upf ${t}: ${rc} ${rc2}\n${out}${err}--- from the saved file\n${out2}${err2}")
  endif()
endforeach()
# save_upf -scope: only what ran in that scope, relative to it
file(READ ${D}/logic.upf base)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/save_scope.upf "${base}set_scope u_core\ncreate_logic_net zz\nset_scope ..\nsave_upf saved_scope.upf -scope u_core\n")
execute_process(COMMAND ${MSIM} ${D}/logic.sv -upf ${CMAKE_CURRENT_BINARY_DIR}/save_scope.upf
                RESULT_VARIABLE rc OUTPUT_QUIET ERROR_VARIABLE err)
file(READ ${CMAKE_CURRENT_BINARY_DIR}/saved_scope.upf saved)
if(NOT rc EQUAL 0 OR NOT saved MATCHES "\ncreate_logic_net zz\n" OR saved MATCHES "create_power_domain")
  message(FATAL_ERROR "save_upf -scope u_core: ${rc} ${err}\n${saved}")
endif()

# Table 8 (5.6): attributes only for models or only for instances; a bad
# value of an HDL attribute.
upf_error_in(hdlattr e_t8a "set_port_attributes -ports {u_c/y} -is_isolated" "UPF_is_isolated can only be given for model ports, with -model (5.6, Table 8)")
upf_error_in(hdlattr e_t8b "set_port_attributes -model core -ports {y} -driver_supply SS" "UPF_driver_supply can only be given for instance ports (5.6, Table 8)")
upf_error_in(hdlattr e_t8c "set_design_attributes -elements {u_m} -is_hard_macro TRUE" "UPF_is_hard_macro can only be given for models, with -models (5.6, Table 8)")
upf_error_in(hdlattr e_t8d "set_design_attributes -models {macro} -attribute {UPF_driver_supply SS}" "UPF_driver_supply can only be given for instances (5.6, Table 8)")
file(READ ${D}/hdlattr.sv hdl)
string(REPLACE "(* UPF_clamp_value = \"1\" *) output logic [3:0] y" "(* UPF_is_analog = \"maybe\" *) output logic [3:0] y" hdl "${hdl}")
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/hdlattr_bad.sv "${hdl}")
execute_process(COMMAND ${MSIM} ${CMAKE_CURRENT_BINARY_DIR}/hdlattr_bad.sv -upf ${D}/hdlattr.upf
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(rc EQUAL 0 OR NOT err MATCHES "hdlattr_bad.sv:12: attribute UPF_is_analog cannot be \"maybe\" [(]5.6[)]")
  message(FATAL_ERROR "a bad HDL attribute was not reported: ${rc}\n${out}${err}")
endif()

# Errors of create_abstract_power_source.
upf_error_in(source e_ps1 "create_abstract_power_source a/b -output_supply_set {out SS_CORE}" "a power source name is a simple name (6.16)")
upf_error_in(source e_ps2 "create_abstract_power_source P2 -output_supply_set {out SS_CORE} -input_supply_set {i SS_HI} -on_state {ON j {1}}" "-on_state ON: j is not an input supply set of P2")
upf_error_in(source e_ps3 "create_abstract_power_source P3 -output_supply_set {out SS_CORE} -input_supply_set {i SS_HI} -control_port {c hi} -on_state {ON i {hi}}" "hi is not a control port of power source P3 (6.16)")
upf_error_in(source e_ps4 "create_abstract_power_source P4 -output_supply_set {out SS_CORE} -input_supply_set {i SS_HI} -ack_port {a ack}" "power source P4 has ack ports but no -supply_set (6.16)")
upf_error_in(source e_ps5 "create_abstract_power_source PS -update -ack_port {a ack}" "create_abstract_power_source -update only adds -power_switch (6.16)")
upf_error_in(source e_ps6 "create_abstract_power_source P5 -output_supply_set {out} -input_supply_set {i SS_HI}" "supply set handle out is not associated with a supply set that has functions (6.16)")
upf_error_in(source e_ps7 "create_power_switch SW -output_supply_port {o VCORE} -input_supply_port {i VHI} -control_port {c lo} -on_state {on i {c}}\ncreate_abstract_power_source PS -update -power_switch SW" "power switch SW of power source PS does not use its control hi (6.16)")

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

# Errors of UPF libraries and VCMs. The copies of vcm.upf read vcm_lib.upf
# next to them.
file(COPY ${D}/vcm_lib.upf DESTINATION ${CMAKE_CURRENT_BINARY_DIR})
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/vcm_bad_lib.upf "create_vcm X -vcms {vlib::REAL}\n")
upf_error_in(vcm e_vcm1 "create_vcm V1" "create_vcm needs one of -table, -function, -model or -vcms (6.30)")
upf_error_in(vcm e_vcm2 "create_vcm V2 -function p::f -model m" "only one of -table, -function, -model and -vcms (6.30)")
upf_error_in(vcm e_vcm3 "create_vcm V3 -function p::f -hdl_type {sv real}" "create_vcm -hdl_type is only for -table (6.30)")
upf_error_in(vcm e_vcm4 "create_vcm V4 -hdl_type {sv real} -conversion_direction hdl2upf -table {{{0 1} {OFF}} {{0.5 2} {FULL_ON}}}" "-table rows 1 and 2 overlap (6.30.3)")
upf_error_in(vcm e_vcm5 "create_vcm V5 -hdl_type {sv real} -conversion_direction upf2hdl -table {{{FULL_ON} {*}} {{OFF} {0}}}" "-table has no row for the supply state PARTIAL_ON (6.30.4)")
upf_error_in(vcm e_vcm6 "create_vcm V6 -hdl_type {sv real} -conversion_direction hdl2upf -table {{{* *} {OFF}}}" "{* *} is not a range (6.30.3)")
upf_error_in(vcm e_vcm7 "create_vcm V7 -hdl_type {sv integer} -conversion_direction upf2hdl -table {{{FULL_ON} {*}} {{OFF} {0}} {{PARTIAL_ON} {0}} {{UNDETERMINED} {0}}}" "'*' is not an integer (6.30.5)")
upf_error_in(vcm e_vcm8 "create_vcm V8 -hdl_type {sv logic} -conversion_direction hdl2upf -table {{0 OFF} {1 FULL_ON}}" "-table has no row for the HDL value X (6.30.7)")
upf_error_in(vcm e_vcm9 "create_vcm V9 -hdl_type {sv real} -conversion_direction hdl2upf -table {{{1 2} {ON}}}" "'ON' is not a value of upfSupplyStateE")
upf_error_in(vcm e_vcm10 "create_vcm R2U -vcms {vlib::R2U}\nconnect_supply_net VR -ports {u_ana/vr} -vcm R2U" "VCM R2U is in UPF libraries IEEE and vlib")
upf_error_in(vcm e_vcm11 "create_vcm V11 -vcms {vlib::R2U vlib::REAL}" "both convert from HDL type real (6.30)")
upf_error_in(vcm e_vcm12 "create_vcm V12 -vcms {nope}" "no VCM 'nope' for -vcms (6.30)")
upf_error_in(vcm e_vcm13 "create_supply_net VX\nconnect_supply_net VX -ports {u_ana/vi} -vcm REAL" "is an integer port (input), and VCM vlib::REAL has no upf2hdl conversion for it")
upf_error_in(vcmfn e_fn1 "create_vcm X -function vcm_pkg::nope\ncreate_supply_net VX\nconnect_supply_net VX -ports {u_ana/vr} -vcm X" "VCM IEEE::X: no function vcm_pkg::nope (6.30.8)")
upf_error_in(vcmfn e_fn2 "create_vcm X -function nopkg::f\ncreate_supply_net VX\nconnect_supply_net VX -ports {u_ana/vr} -vcm X" "no package nopkg for -function nopkg::f (6.30.8)")
upf_error_in(vcmfn e_fn3 "create_vcm X -function vcm_pkg::both\ncreate_supply_net VX\nconnect_supply_net VX -ports {u_ana/vr} -vcm X" "exactly one of the argument and the return type of vcm_pkg::both is upfSupplyTypeT (6.30.8)")
upf_error_in(vcmfn e_fn4 "create_vcm X -function vcm_pkg::two\ncreate_supply_net VX\nconnect_supply_net VX -ports {u_ana/vr} -vcm X" "vcm_pkg::two needs exactly one input argument (6.30.8)")
upf_error_in(vcmfn e_fn5 "create_vcm X -model nomod\ncreate_supply_net VX\nconnect_supply_net VX -ports {u_ana/vr} -vcm X" "no module nomod for -model (6.30.9)")
upf_error_in(vcmfn e_fn6 "create_vcm X -model two_in\ncreate_supply_net VX\nconnect_supply_net VX -ports {u_ana/vr} -vcm X" "two_in needs one input and one output port (6.30.9)")
upf_error_in(vcmfn e_fn7 "create_supply_net VX\nconnect_supply_net VX -ports {u_ana/vi} -vcm FN" "is an integer port (input), and VCM IEEE::FN has no upf2hdl conversion for it")
upf_error_in(vcm e_vcm15 "create_vcm V15 -vcms {.::R2U}" "'.' names the library being created, and this is not in create_upf_library (6.29)")
upf_error_in(vcm e_lib1 "create_upf_library L1 -contents {create_supply_net X}" "create_supply_net inside create_upf_library L1; a UPF library holds only create_vcm and use_upf_library (6.29)")
upf_error_in(vcm e_lib2 "create_upf_library vlib -contents {}" "UPF library vlib already exists (6.29)")
upf_error_in(vcm e_lib3 "use_upf_library nolib" "use_upf_library: no UPF library nolib (6.63)")
upf_error_in(vcm e_lib4 "load_upf_library vcm_bad_lib.upf" "create_vcm in a UPF library file; only create_upf_library is allowed (6.36)")
upf_error_in(vcm e_lib5 "connect_supply_net VR -ports {u_ana/vr} -vcm zlib::R2U" "no UPF library zlib (5.11)")
upf_error_in(vcm e_lib6 "connect_supply_net VR -ports {u_ana/vr} -vcm vlib::NOPE" "UPF library vlib has no VCM NOPE (6.30)")
upf_error_in(vcm e_lib7 "create_upf_library L7 -contents {create_vcm A -vcms {vlib::R2U}}\nconnect_supply_net VR -ports {u_ana/vr} -vcm A" "no VCM 'A'")

# Errors of connect_supply_net by pg_type.
upf_error_in(pgconn e_pg1 "connect_supply_net VBAT -ports {u_m1/VBK} -pg_type backup_power" "-ports cannot go with -pg_type, -cells, -domain or -elements (6.14)")
upf_error_in(pgconn e_pg2 "connect_supply_net VBAT -pg_type backup_power" "-pg_type needs -cells, -domain or -elements (6.14)")
upf_error_in(pgconn e_pg3 "connect_supply_net VBAT" "connect_supply_net needs -ports or -pg_type (6.14)")
upf_error_in(pgconn e_pg4 "connect_supply_net VBAT -pg_type backup_power -cells nocell" "connect_supply_net -cells: no instance of nocell in the design (6.14)")
upf_error_in(pgconn e_pg5 "connect_supply_net VBAT -pg_type backup_power -domain PD_X" "connect_supply_net -domain: no power domain PD_X")
upf_error_in(pgconn e_pg6 "connect_supply_net VBAT -pg_type backup_power -elements {u_m9}" "connect_supply_net -elements: no instance u_dut.u_m9 in the design (6.14)")
