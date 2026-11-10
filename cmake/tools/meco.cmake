# meco: functional ECO (Conformal ECO style). See docs/ECO.md.
#   meco -old old.v -new new.v -lib cells.lib -o patched.v -script eco.tcl
# Netlists in, so no slang: it uses the shared netlist and Liberty readers,
# the equivalence checker's AIG and SAT, and the shared thread pool.
find_package(Threads REQUIRED)
add_executable(meco src/meco.cpp)
target_include_directories(meco PRIVATE src)
target_link_libraries(meco PRIVATE Threads::Threads)
install(TARGETS meco RUNTIME DESTINATION bin)

add_test(NAME meco
         COMMAND ${CMAKE_COMMAND} -DMECO=$<TARGET_FILE:meco> -DSRC=${CMAKE_SOURCE_DIR}
                 -DOUT=${CMAKE_CURRENT_BINARY_DIR}/meco_test -P ${CMAKE_SOURCE_DIR}/tests/meco.cmake)

# Random ECOs. meco proves each patch; the slang build also simulates the
# patched netlist against the new one in msim.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
  set(MECO_FUZZ ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/eco/fuzz.py $<TARGET_FILE:meco>)
  set(MECO_FUZZ_LIBS ${CMAKE_SOURCE_DIR}/tests/sta/sta_test.lib,${CMAKE_SOURCE_DIR}/tests/eco/eco_cells.lib
                     ${CMAKE_SOURCE_DIR}/tests/eco/cells.v)
  add_test(NAME meco_fuzz_prove
           COMMAND ${MECO_FUZZ} - ${MECO_FUZZ_LIBS} 100 200 ${CMAKE_CURRENT_BINARY_DIR}/meco_fuzz_prove)
  if(MINISIM_WITH_SLANG)
    add_test(NAME meco_fuzz
             COMMAND ${MECO_FUZZ} $<TARGET_FILE:msim> ${MECO_FUZZ_LIBS} 0 40 ${CMAKE_CURRENT_BINARY_DIR}/meco_fuzz)
  endif()
endif()
