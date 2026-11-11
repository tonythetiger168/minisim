# sky130: cells of the SkyWater 130 nm library as a second test library
# (lib/sky130/README.md). Test data only: nothing is built or installed.
include_guard(GLOBAL)

# mlibc on the subset and its LEF, then RTL to a sky130 netlist with msynth
# and timing and power on it.
if(TARGET mlibc)
  add_test(NAME sky130
           COMMAND ${CMAKE_COMMAND} -DBIN=${CMAKE_BINARY_DIR} -DSRC=${CMAKE_SOURCE_DIR}
                   -DWORK=${CMAKE_BINARY_DIR}/sky130_test -DFULL=0
                   -P ${CMAKE_SOURCE_DIR}/tests/sky130.cmake)
  # The whole library, when `lib/sky130/sky130.py fetch` has been run.
  if(EXISTS ${CMAKE_SOURCE_DIR}/lib/sky130/pdk/sky130_fd_sc_hd__tt_025C_1v80.lib)
    add_test(NAME sky130_full
             COMMAND ${CMAKE_COMMAND} -DBIN=${CMAKE_BINARY_DIR} -DSRC=${CMAKE_SOURCE_DIR}
                     -DWORK=${CMAKE_BINARY_DIR}/sky130_test_full -DFULL=1
                     -P ${CMAKE_SOURCE_DIR}/tests/sky130.cmake)
  endif()
endif()
