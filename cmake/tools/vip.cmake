# The VIP library (docs/VIP.md): UVM verification IP in vip/, its self-test
# benches in tests/vip, and mvip, which runs open-vip-portfolio on msim.
include_guard(GLOBAL)
install(DIRECTORY vip DESTINATION ${CMAKE_INSTALL_DATADIR}/minisim)
install(PROGRAMS tools/mvip.py DESTINATION ${CMAKE_INSTALL_BINDIR} RENAME mvip)
install(FILES docs/VIP.md DESTINATION ${CMAKE_INSTALL_DOCDIR})

# Each tests/vip/<name>/files.f is a self-test bench; its "//! top:" line
# names the top module and each "//! test:" line a UVM test to run. They
# need msim (the slang front end) and the Accellera UVM library (UVM_HOME).
if(MINISIM_WITH_SLANG AND UVM_HOME AND EXISTS ${UVM_HOME}/src/uvm_pkg.sv)
  file(GLOB VIP_BENCHES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tests/vip/*/files.f)
  foreach(f IN LISTS VIP_BENCHES)
    get_filename_component(dir ${f} DIRECTORY)
    get_filename_component(bench ${dir} NAME)
    file(STRINGS ${f} flines)
    set(vtop "")
    foreach(l IN LISTS flines)
      if(l MATCHES "^//! top: *([A-Za-z0-9_]+)")
        set(vtop ${CMAKE_MATCH_1})
      elseif(l MATCHES "^//! test: *([A-Za-z0-9_]+)")
        add_test(NAME vip/${bench}/${CMAKE_MATCH_1}
                 COMMAND ${CMAKE_COMMAND} -DMSIM=$<TARGET_FILE:msim> -DUVM_HOME=${UVM_HOME}
                         -DROOT=${CMAKE_SOURCE_DIR} -DF=${f} -DTOP=${vtop} -DTEST=${CMAKE_MATCH_1}
                         -P ${CMAKE_SOURCE_DIR}/tests/vip/run_vip.cmake)
        set_tests_properties(vip/${bench}/${CMAKE_MATCH_1} PROPERTIES LABELS "vip")
      endif()
    endforeach()
  endforeach()
endif()
