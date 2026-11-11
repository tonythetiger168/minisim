# meco: functional ECO (Conformal ECO style). See docs/ECO.md.
#   meco -old old.v -new new.v -lib cells.lib -o patched.v -script eco.tcl
# Netlists in. It uses the shared netlist and Liberty readers, the
# equivalence checker's AIG, the shared SAT solver (the `cadical` target,
# which comes with the slang build) and the shared thread pool.
include_guard(GLOBAL)

if(MINISIM_WITH_SLANG)
  find_package(Threads REQUIRED)
  add_executable(meco src/meco.cpp)
  target_include_directories(meco PRIVATE src)
  target_link_libraries(meco PRIVATE cadical Threads::Threads)
  install(TARGETS meco RUNTIME DESTINATION bin)

  add_test(NAME meco
           COMMAND ${CMAKE_COMMAND} -DMECO=$<TARGET_FILE:meco> -DSRC=${CMAKE_SOURCE_DIR}
                   -DOUT=${CMAKE_CURRENT_BINARY_DIR}/meco_test -P ${CMAKE_SOURCE_DIR}/tests/meco.cmake)

  # Random ECOs: meco proves each patch, and msim simulates the patched
  # netlist against the new one.
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_FOUND)
    add_test(NAME meco_fuzz
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/eco/fuzz.py $<TARGET_FILE:meco> $<TARGET_FILE:msim>
                     ${CMAKE_SOURCE_DIR}/tests/sta/sta_test.lib,${CMAKE_SOURCE_DIR}/tests/eco/eco_cells.lib
                     ${CMAKE_SOURCE_DIR}/tests/eco/cells.v 0 200 ${CMAKE_CURRENT_BINARY_DIR}/meco_fuzz)
  endif()
endif()
