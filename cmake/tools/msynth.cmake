# msynth: logic synthesis with a dc_shell-style Tcl shell (docs/SYNTH.md).
#   msynth -f run.tcl
# Needs the slang front end; included from the top CMakeLists.txt, once
# (a second include, for instance from a glob of this directory, does nothing).
include_guard(GLOBAL)
if(NOT MINISIM_WITH_SLANG)
  return()
endif()
include(GNUInstallDirs)
find_package(Python3 COMPONENTS Interpreter)

add_executable(msynth src/msynth.cpp)
target_include_directories(msynth PRIVATE src)
# The default target library, mini45, is found next to an installed msynth
# (share/minisim/lib/mini45) or, for a build-tree msynth, in the source tree.
target_compile_definitions(msynth PRIVATE MSYNTH_ROOT_DIR="${CMAKE_SOURCE_DIR}")
find_package(Threads REQUIRED)
target_link_libraries(msynth PRIVATE slang::slang Threads::Threads)
install(TARGETS msynth RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# The memory table of the shared front half (aig::Design::mems): a model built
# from the table alone must track the memory's flops and read ports.
add_executable(test_aig_mem tests/test_aig_mem.cpp)
target_include_directories(test_aig_mem PRIVATE src)
target_compile_options(test_aig_mem PRIVATE -UNDEBUG)
target_link_libraries(test_aig_mem PRIVATE slang::slang Threads::Threads)
add_test(NAME test_aig_mem COMMAND test_aig_mem ${CMAKE_SOURCE_DIR}/tests/synth/mem.sv)

# Each design is synthesized, and its testbench is simulated with msim on the
# RTL and on the netlist: both must print the same. Once with the default
# library (mini45) and once with a library of only an inverter, a NAND and one flop.
if(Python3_FOUND)
  foreach(t counter alu fsm hier mem)
    add_test(NAME msynth_${t}
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/synth/run_test.py
                     $<TARGET_FILE:msynth> $<TARGET_FILE:msim> ${CMAKE_SOURCE_DIR}/tests/synth
                     ${CMAKE_CURRENT_BINARY_DIR}/msynth_test ${t})
    add_test(NAME msynth_${t}_nand
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/synth/run_test.py
                     $<TARGET_FILE:msynth> $<TARGET_FILE:msim> ${CMAKE_SOURCE_DIR}/tests/synth
                     ${CMAKE_CURRENT_BINARY_DIR}/msynth_test_nand ${t} ${CMAKE_SOURCE_DIR}/tests/synth/nand.lib)
  endforeach()
  # The netlist is also proven equivalent to the RTL by mequiv, and a netlist
  # with one gate swapped must be reported as different.
  if(TARGET mequiv)
    foreach(t counter alu fsm hier)
      add_test(NAME msynth_equiv_${t}
               COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/synth/equiv_test.py
                       $<TARGET_FILE:msynth> $<TARGET_FILE:mequiv> ${CMAKE_SOURCE_DIR}/tests/synth
                       ${CMAKE_CURRENT_BINARY_DIR}/msynth_test ${t})
    endforeach()
  endif()
  add_test(NAME msynth_shell
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/synth/shell_test.py
                   $<TARGET_FILE:msynth> ${CMAKE_SOURCE_DIR}/tests/synth ${CMAKE_CURRENT_BINARY_DIR}/msynth_shell)
endif()
