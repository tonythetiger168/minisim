# Run one spec test: simulate SV with SIM and compare stdout with the file's
# expected lines. An expected line is a comment that starts with "//> ".
#   cmake -DSIM=<msim|iverilog> -DSV=<file.sv> [-DVVP=<vvp>] -P run_spec.cmake
# "//! args: <words>" adds msim command-line arguments (more source files,
# -libmap, -sdf ...), relative to the test's directory.
# With -DVVP set, SIM is iverilog: compile with -g2012, then run under vvp and
# drop vvp's own "$finish called" / "VCD info" lines.
# Each test runs in its own directory, so a data file is a relative path.
# "//! icarus-vpi: <file.c> ..." builds those C files into a VPI module
# with iverilog-vpi for the Icarus run (iverilog -m names it, vvp -M finds it).
# -DTIMEOUT=<s>: how long the run may take (default 20 s); the datapath
# benches, which sweep whole input spaces, pass more so a loaded machine
# (ctest -j8) does not time them out.
if(NOT DEFINED TIMEOUT)
  set(TIMEOUT 20)
endif()
get_filename_component(dir ${SV} DIRECTORY)
get_filename_component(base ${SV} NAME)   # run as a relative name, so `__FILE__ and messages are stable
file(STRINGS ${SV} lines)
set(expected "")
set(extra "")
set(vpi_src "")
set(want_rc 0)   # "//! exit: <n>": the exit code msim should return (e.g. 1 after $fatal)
foreach(l IN LISTS lines)
  if(l MATCHES "^//> (.*)$")
    string(APPEND expected "${CMAKE_MATCH_1}\n")
  elseif(l STREQUAL "//>")
    string(APPEND expected "\n")
  elseif(l MATCHES "^//! exit: ([0-9]+)")
    set(want_rc ${CMAKE_MATCH_1})
  elseif(l MATCHES "^//! args: (.*)$")   # more msim arguments, e.g. a DPI C file
    separate_arguments(extra UNIX_COMMAND "${CMAKE_MATCH_1}")
  elseif(l MATCHES "^//! icarus-vpi: (.*)$")
    separate_arguments(vpi_src UNIX_COMMAND "${CMAKE_MATCH_1}")
  endif()
endforeach()

if(VVP)
  get_filename_component(name ${SV} NAME)  # NAME_WE would cut 6.7.1-x at "6"
  set(out ${CMAKE_CURRENT_BINARY_DIR}/icarus_${name}.vvp)
  set(vpi_args "")
  if(vpi_src)
    get_filename_component(ivbin ${SIM} DIRECTORY)
    set(vdir ${CMAKE_CURRENT_BINARY_DIR}/icarus_${name}.vpidir)
    file(MAKE_DIRECTORY ${vdir})
    set(srcs "")
    foreach(c IN LISTS vpi_src)
      list(APPEND srcs ${dir}/${c})
    endforeach()
    execute_process(COMMAND ${ivbin}/iverilog-vpi --name=tb ${srcs} -I${CMAKE_CURRENT_LIST_DIR}/../../include
                    WORKING_DIRECTORY ${vdir} RESULT_VARIABLE rc OUTPUT_VARIABLE o ERROR_VARIABLE err)
    if(NOT rc EQUAL 0)
      message(FATAL_ERROR "iverilog-vpi failed on ${vpi_src}:\n${o}${err}")
    endif()
    set(vpi_args -L${vdir} -mtb)
  endif()
  execute_process(COMMAND ${SIM} -g2012 ${vpi_args} -o ${out} ${SV}
                  RESULT_VARIABLE rc ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "iverilog failed on ${SV}:\n${err}")
  endif()
  set(vvp_args "")
  if(vpi_src)
    set(vvp_args -M${vdir})   # the compile named the module (-m), so vvp loads it once
  endif()
  set(plus "")   # +plusargs from "//! args:" go to the run, as for msim
  foreach(a IN LISTS extra)
    if(a MATCHES "^\\+")
      list(APPEND plus ${a})
    endif()
  endforeach()
  execute_process(COMMAND ${VVP} -n ${vvp_args} ${out} ${plus} OUTPUT_VARIABLE got RESULT_VARIABLE rc TIMEOUT ${TIMEOUT}
                  WORKING_DIRECTORY ${dir})
  string(REGEX REPLACE "[^\n]*\\$finish called[^\n]*\n" "" got "${got}")
  string(REGEX REPLACE "VCD info:[^\n]*\n" "" got "${got}")
  string(REGEX REPLACE "WARNING: [^\n]*\n" "" got "${got}")   # vvp's warnings go to stdout
else()
  execute_process(COMMAND ${SIM} ${base} ${extra} OUTPUT_VARIABLE got ERROR_VARIABLE err
                  RESULT_VARIABLE rc TIMEOUT ${TIMEOUT} WORKING_DIRECTORY ${dir})
  if(NOT rc STREQUAL want_rc)
    message(FATAL_ERROR "msim exited ${rc} (expected ${want_rc}) on ${SV}:\n${err}")
  endif()
endif()

if(NOT got STREQUAL expected)
  message(FATAL_ERROR "output differs for ${SV}\n--- expected\n${expected}--- got\n${got}")
endif()
