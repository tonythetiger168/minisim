# Which tests this configuration does NOT register, and why.
#
# A number of tests only exist when an optional program or an optional data
# set is present: Icarus for the icarus/* cross-checks, an uvm-core checkout
# for uvm/*, GHDL for the VHDL goldens, and so on. CMake registers them
# silently or not at all, so a "full local suite" can be green while hundreds
# of tests were never built. Two sky130 and 130 Icarus tests hid that way.
#
# This file is included last. It re-checks each gate, prints a summary that is
# hard to miss, and writes build/skipped-tests.txt. Add a line here whenever
# you add a test that depends on something optional.
include_guard(GLOBAL)

set(MINISIM_GATES "")
# minisim_gate(<label> <what is missing> <how to get it> <condition...>)
# The condition is an ordinary if() expression; when it is false, the label is
# reported as not registered.
macro(minisim_gate label missing fix)
  if(${ARGN})
  else()
    list(APPEND MINISIM_GATES "${label}\t${missing}\t${fix}")
  endif()
endmacro()

minisim_gate("icarus/* (one per tests/spec/*.sv)" "Icarus Verilog (iverilog, vvp)"
             "apt-get install iverilog, or build v13_0 and configure -DIVERILOG=... -DVVP=..."
             IVERILOG AND VVP)
minisim_gate("mregress_spec_icarus" "Icarus Verilog (iverilog, vvp)"
             "as above" IVERILOG AND VVP)
minisim_gate("uvm/* and mdebug_uvm_tr" "an Accellera uvm-core checkout"
             "configure -DUVM_HOME=<uvm-core 2020.3.1 checkout>"
             UVM_HOME AND EXISTS "${UVM_HOME}/src/uvm_pkg.sv")
minisim_gate("vip/*" "an Accellera uvm-core checkout" "as above"
             UVM_HOME AND EXISTS "${UVM_HOME}/src/uvm_pkg.sv")
minisim_gate("test_protect" "OpenSSL" "apt-get install libssl-dev" OPENSSL_FOUND)
minisim_gate("protect_interop" "the openssl command line" "apt-get install openssl"
             OPENSSL_FOUND AND MINISIM_OPENSSL_CLI AND UNIX AND MINISIM_WITH_SLANG)
minisim_gate("mlint_tidy" "a slang-tidy checkout" "configure -DTIDY_DIR=<slang checkout>"
             MINISIM_WITH_SLANG_TIDY AND TIDY_DIR)
minisim_gate("dw_fp/*, msupport, mbug and the Python-driven tests" "a Python 3 interpreter"
             "apt-get install python3" Python3_FOUND)
minisim_gate("the slang front end and every msim test" "the slang front end"
             "configure -DMINISIM_WITH_SLANG=ON" MINISIM_WITH_SLANG)

# GHDL and node are used inside tests that always register; they degrade
# rather than disappear, which is its own quiet hole, so they are listed too.
find_program(GHDL ghdl)
find_program(NODE node)
minisim_gate("vhdl/* compared with GHDL (the tests run either way)"
             "GHDL, the VHDL reference"
             "apt-get install ghdl (external program only: GPL)" GHDL)
minisim_gate("mdebug web viewer checks in node (the tests run either way)"
             "node" "apt-get install nodejs" NODE)

if(MINISIM_GATES)
  list(LENGTH MINISIM_GATES n)
  string(REPEAT "=" 70 bar)
  message(STATUS "${bar}")
  message(STATUS "minisim: ${n} group(s) of tests are NOT registered in this build.")
  message(STATUS "A green ctest run here does NOT cover them.")
  foreach(g ${MINISIM_GATES})
    string(REPLACE "\t" ";" g "${g}")
    list(GET g 0 label)
    list(GET g 1 missing)
    list(GET g 2 fix)
    message(STATUS "  - ${label}")
    message(STATUS "      needs: ${missing}")
    message(STATUS "      fix:   ${fix}")
  endforeach()
  message(STATUS "The same list is in ${CMAKE_BINARY_DIR}/skipped-tests.txt")
  message(STATUS "${bar}")
  string(REPLACE ";" "\n" out "${MINISIM_GATES}")
  file(WRITE ${CMAKE_BINARY_DIR}/skipped-tests.txt "${out}\n")
else()
  message(STATUS "minisim: every optional test group is registered in this build.")
  file(WRITE ${CMAKE_BINARY_DIR}/skipped-tests.txt "")
endif()

# An always-registered test, so `ctest` itself names the holes instead of the
# run looking complete. It passes: a missing optional tool is not a failure.
add_test(NAME test_gates
         COMMAND ${CMAKE_COMMAND} -DFILE=${CMAKE_BINARY_DIR}/skipped-tests.txt
                 -P ${CMAKE_SOURCE_DIR}/tests/gates.cmake)
set_tests_properties(test_gates PROPERTIES LABELS "meta")
