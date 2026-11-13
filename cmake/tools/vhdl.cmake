# msim's VHDL front end (src/vhdl/, docs/VHDL.md): VHDL tests whose expected
# output was made with GHDL, mixed SystemVerilog/VHDL tests, and the vhdl2sv
# debug aid that prints the SystemVerilog msim simulates.
include_guard(GLOBAL)
add_executable(vhdl2sv ${CMAKE_SOURCE_DIR}/src/vhdl2sv.cpp)
target_include_directories(vhdl2sv PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(TARGET msim)
  set(VT ${CMAKE_SOURCE_DIR}/tests/vhdl)
  # top | files | "ghdl" to re-check the golden with GHDL | data files | exit code
  set(VHDL_TESTS
    "counter_tb|counter.vhd,counter_tb.vhd|ghdl"
    "pkg_types|pkg_types.vhd|ghdl"
    "structure|structure.vhd|ghdl"
    "seq|seq.vhd|ghdl"
    "textio_math|textio_math.vhd|ghdl|textio_math.txt|1"
    "rom_records|rom_records.vhd|ghdl"
    "protected_tb|protected.vhd|ghdl"
    "access_tb|access.vhd|ghdl"
    "generic_pkg_tb|generic_pkg.vhd|ghdl"
    "mixed_sv_top|counter.vhd,mixed_sv_top.sv|"
    "mixed_vhdl_top|mixed_adder.sv,mixed_vhdl_top.vhd|")
  foreach(t ${VHDL_TESTS})
    string(REPLACE "|" ";" parts "${t}")
    list(GET parts 0 top)
    list(GET parts 1 files)
    list(GET parts 2 ghdl)
    list(LENGTH parts np)
    set(data "")
    set(rcarg "")
    if(np GREATER 3)
      list(GET parts 3 data)
    endif()
    if(np GREATER 4)
      list(GET parts 4 wrc)
      set(rcarg -DWANT_RC=${wrc})
    endif()
    string(REPLACE "," ";" files "${files}")
    add_test(NAME vhdl_${top}
             COMMAND ${CMAKE_COMMAND} -DMSIM=$<TARGET_FILE:msim> -DSRC=${VT} -DTOP=${top} "-DFILES=${files}"
                     "-DDATA=${data}" ${rcarg} -DOUT=${CMAKE_CURRENT_BINARY_DIR}/vhdl_${top} -P ${VT}/run_vhdl.cmake)
    if(ghdl STREQUAL "ghdl")
      add_test(NAME vhdl_ghdl_${top}
               COMMAND ${CMAKE_COMMAND} -DSRC=${VT} -DTOP=${top} "-DFILES=${files}"
                       "-DDATA=${data}" -DOUT=${CMAKE_CURRENT_BINARY_DIR}/vhdl_ghdl_${top} -P ${VT}/ghdl_check.cmake)
    endif()
  endforeach()
endif()
