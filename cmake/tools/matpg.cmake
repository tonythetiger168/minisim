# matpg: test pattern generation and fault simulation on a scan-inserted gate
# netlist (docs/ATPG.md). Plain C++20 and threads: no slang, so it builds in
# every configuration.
include_guard(GLOBAL)
find_package(Threads REQUIRED)
add_executable(matpg src/matpg.cpp)
target_include_directories(matpg PRIVATE src)
target_link_libraries(matpg PRIVATE Threads::Threads)

# The fault simulator (src/faultsim.hpp) and PODEM (src/atpg.hpp) against
# brute force on random circuits.
add_executable(test_faultsim tests/test_faultsim.cpp)
target_include_directories(test_faultsim PRIVATE src)
target_compile_options(test_faultsim PRIVATE -UNDEBUG)
target_link_libraries(test_faultsim PRIVATE Threads::Threads)
add_test(NAME test_faultsim COMMAND test_faultsim)

# The command line on tests/atpg: coverage, outputs, thread counts.
add_test(NAME matpg
         COMMAND ${CMAKE_COMMAND} -DMATPG=$<TARGET_FILE:matpg> -DSRC=${CMAKE_SOURCE_DIR}
                 -DOUT=${CMAKE_CURRENT_BINARY_DIR}/matpg_test -P ${CMAKE_SOURCE_DIR}/tests/matpg.cmake)

# The patterns replayed in msim (and Icarus when present): they pass on the
# design and fail on a copy with a detected fault wired in.
if(MINISIM_WITH_SLANG)
  find_program(IVERILOG iverilog)
  find_program(VVP vvp)
  add_test(NAME matpg_sim
           COMMAND ${CMAKE_COMMAND} -DMATPG=$<TARGET_FILE:matpg> -DSRC=${CMAKE_SOURCE_DIR}
                   -DOUT=${CMAKE_CURRENT_BINARY_DIR}/matpg_sim -DMSIM=$<TARGET_FILE:msim>
                   -DIVERILOG=${IVERILOG} -DVVP=${VVP} -P ${CMAKE_SOURCE_DIR}/tests/matpg.cmake)
endif()

include(GNUInstallDirs)
install(TARGETS matpg RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES docs/ATPG.md DESTINATION ${CMAKE_INSTALL_DOCDIR})
