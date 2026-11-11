# Power-aware simulation (msim -upf) and UPF checks (mlint -upf), IEEE 1801-2024.
#   cmake -DMSIM=<msim> -DMLINT=<mlint> -DSRC=<source dir> -P upf.cmake
# Each tests/upf/<name>.sv runs with <name>.upf and must print <name>.out.
set(D ${SRC}/tests/upf)
foreach(t gating retention states)
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
