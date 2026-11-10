# mfpga: FPGA synthesis (LUT mapping, nextpnr JSON and EDIF output).
#   mfpga design.sv -top core -family ice40 -o core.json
# Needs the slang front end; included by the cmake/tools glob.
include_guard(GLOBAL)
if(NOT MINISIM_WITH_SLANG)
  return()
endif()
include(GNUInstallDirs)
find_package(Python3 COMPONENTS Interpreter)
add_executable(mfpga src/mfpga.cpp)
target_include_directories(mfpga PRIVATE src)
find_package(Threads REQUIRED)
target_link_libraries(mfpga PRIVATE slang::slang Threads::Threads)
install(TARGETS mfpga RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# Each design (the ones msynth is tested with, a datapath with synchronous
# reset, enable, adders and compares, and an AIGER file with a flop that
# powers up 1) is mapped for every family: checked by random simulation,
# for equal output with 1 and 4 threads, by simulating the design's testbench
# with msim on the RTL and on the mapped netlist, and, when yosys and nextpnr
# are installed, by a bounded equivalence proof against the RTL and by place
# and route. name:cycles[:reset] gives the cycles of the proof (0: none; Yosys
# 0.33 cannot read fsm.sv and hier.sv) and the input it holds at 1 first.
if(Python3_FOUND)
  foreach(t counter:12 fsm:0 hier:0 alu:0 latch1:0 dp:6:rst)
    string(REPLACE ":" ";" t_parts ${t})
    list(GET t_parts 0 t_name)
    list(GET t_parts 1 t_depth)
    list(LENGTH t_parts t_len)
    set(t_reset "")
    if(t_len GREATER 2)
      list(GET t_parts 2 t_reset)
    endif()
    add_test(NAME mfpga_${t_name}
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/fpga/run_test.py
                     $<TARGET_FILE:mfpga> $<TARGET_FILE:msim> ${CMAKE_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/mfpga_test
                     ${t_name} ${t_depth} ${t_reset})
  endforeach()
endif()
