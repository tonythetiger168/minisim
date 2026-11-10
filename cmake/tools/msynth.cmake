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

# The built-in generic cell library is compiled in from lib/msynth_generic.lib.
file(READ ${CMAKE_SOURCE_DIR}/lib/msynth_generic.lib MSYNTH_GENERIC_LIB)
string(LENGTH "${MSYNTH_GENERIC_LIB}" MSYNTH_LIB_LEN)
set(MSYNTH_LIB_CHUNKS "")
set(MSYNTH_LIB_POS 0)
while(MSYNTH_LIB_POS LESS MSYNTH_LIB_LEN)   # compilers limit one string literal: 8K pieces
  string(SUBSTRING "${MSYNTH_GENERIC_LIB}" ${MSYNTH_LIB_POS} 8000 MSYNTH_LIB_PIECE)
  string(APPEND MSYNTH_LIB_CHUNKS "R\"MSLIB(${MSYNTH_LIB_PIECE})MSLIB\"\n")
  math(EXPR MSYNTH_LIB_POS "${MSYNTH_LIB_POS} + 8000")
endwhile()
file(WRITE ${CMAKE_BINARY_DIR}/gen/msynth_generic_lib.hpp.new
     "// Generated from lib/msynth_generic.lib\nstatic const char kGenericLib[] =\n${MSYNTH_LIB_CHUNKS};\n")
execute_process(COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_BINARY_DIR}/gen/msynth_generic_lib.hpp.new ${CMAKE_BINARY_DIR}/gen/msynth_generic_lib.hpp)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/lib/msynth_generic.lib)

add_executable(msynth src/msynth.cpp)
target_include_directories(msynth PRIVATE src)
find_package(Threads REQUIRED)
target_link_libraries(msynth PRIVATE slang::slang Threads::Threads)
install(TARGETS msynth RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# Each design is synthesized, and its testbench is simulated with msim on the
# RTL and on the netlist: both must print the same. Once with the built-in
# library and once with a library of only an inverter, a NAND and one flop.
if(Python3_FOUND)
  foreach(t counter alu fsm hier)
    add_test(NAME msynth_${t}
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/synth/run_test.py
                     $<TARGET_FILE:msynth> $<TARGET_FILE:msim> ${CMAKE_SOURCE_DIR}/tests/synth
                     ${CMAKE_CURRENT_BINARY_DIR}/msynth_test ${t})
    add_test(NAME msynth_${t}_nand
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/synth/run_test.py
                     $<TARGET_FILE:msynth> $<TARGET_FILE:msim> ${CMAKE_SOURCE_DIR}/tests/synth
                     ${CMAKE_CURRENT_BINARY_DIR}/msynth_test_nand ${t} ${CMAKE_SOURCE_DIR}/tests/synth/nand.lib)
  endforeach()
  add_test(NAME msynth_shell
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/synth/shell_test.py
                   $<TARGET_FILE:msynth> ${CMAKE_SOURCE_DIR}/tests/synth ${CMAKE_CURRENT_BINARY_DIR}/msynth_shell)
endif()
