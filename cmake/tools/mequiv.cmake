# mequiv: logic equivalence checking, RTL vs netlist and netlist vs netlist
# (docs/EQUIV.md).   mequiv -ref rtl.sv -impl netlist.v cells.v
include_guard(GLOBAL)

# The SAT solver is CaDiCaL behind src/sat.hpp; its cmake target comes with
# the slang build (CMakeLists.txt).
if(MINISIM_WITH_SLANG)
  # src/sat.hpp and the bit-vector logic and encoding in src/equiv.
  add_executable(test_sat tests/test_sat.cpp)
  target_include_directories(test_sat PRIVATE src)
  target_compile_options(test_sat PRIVATE -UNDEBUG)
  target_link_libraries(test_sat PRIVATE cadical)
  add_test(NAME test_sat COMMAND test_sat)

  find_package(Threads REQUIRED)
  add_executable(mequiv src/mequiv.cpp)
  target_include_directories(mequiv PRIVATE src)
  target_link_libraries(mequiv PRIVATE slang::slang cadical Threads::Threads)

  add_test(NAME mequiv
           COMMAND ${CMAKE_COMMAND} -DMEQUIV=$<TARGET_FILE:mequiv>
                   -DSRC=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/mequiv.cmake)

  include(GNUInstallDirs)
  install(TARGETS mequiv RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
