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

# UPF is Tcl (5.1): the same intent written with proc, foreach, if, expr and
# [command] substitution simulates the same.
execute_process(COMMAND ${MSIM} ${D}/gating.sv -upf ${D}/tcl.upf
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
file(READ ${D}/gating.out want)
if(NOT rc EQUAL 0 OR NOT out STREQUAL want)
  message(FATAL_ERROR "tcl.upf: ${rc}\n${out}${err}")
endif()
# find_objects (6.33): the same intent with its domains, isolation elements and
# level-shifter ports found in the design; the UPF checks every query itself.
foreach(pt "" "tb/u_dut")
  set(args ${D}/gating.sv -upf ${D}/find.upf)
  if(pt)
    list(APPEND args -power_top ${pt})
  endif()
  execute_process(COMMAND ${MSIM} ${args} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  file(READ ${D}/gating.out want)
  if(NOT rc EQUAL 0 OR NOT out STREQUAL want)
    message(FATAL_ERROR "find.upf (power top '${pt}'): ${rc}\n${out}${err}")
  endif()
endforeach()
execute_process(COMMAND ${MLINT} ${D}/gating.sv -upf ${D}/find.upf -only upf
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT out MATCHES "0 findings")
  message(FATAL_ERROR "mlint find.upf is not clean: ${rc}\n${out}${err}")
endif()
# The information model and its queries (clause 10, 11.1): query.upf runs
# after gating.upf and checks every answer itself.
file(READ ${D}/gating.upf g)
file(READ ${D}/query.upf q)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/query_all.upf "${g}${q}")
execute_process(COMMAND ${MSIM} ${D}/gating.sv -upf ${CMAKE_CURRENT_BINARY_DIR}/query_all.upf
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
file(READ ${D}/gating.out want)
if(NOT rc EQUAL 0 OR NOT out STREQUAL want)
  message(FATAL_ERROR "query.upf (msim): ${rc}\n${out}${err}")
endif()
execute_process(COMMAND ${MLINT} ${D}/gating.sv -upf ${CMAKE_CURRENT_BINARY_DIR}/query_all.upf -only upf
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT out MATCHES "0 findings")
  message(FATAL_ERROR "query.upf (mlint): ${rc}\n${out}${err}")
endif()
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/badfind.upf "set_design_top dut\nfind_objects u_nope -pattern *\n")
execute_process(COMMAND ${MSIM} ${D}/gating.sv -upf ${CMAKE_CURRENT_BINARY_DIR}/badfind.upf
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(rc EQUAL 0 OR NOT err MATCHES "badfind.upf:2: find_objects: no scope 'u_dut.u_nope' in the design")
  message(FATAL_ERROR "find_objects on a missing scope was not reported: ${rc}\n${out}${err}")
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
