# One VHDL test: msim runs the files, its output must equal the golden file
# (made with GHDL, an independent VHDL simulator; ghdl_check.cmake re-checks
# the goldens against GHDL when it is installed).
#   cmake -DMSIM=<msim> -DSRC=<tests/vhdl> -DTOP=<top> -DFILES=<a.vhd;b.vhd> -DOUT=<dir> -P run_vhdl.cmake
file(REMOVE_RECURSE ${OUT})
file(MAKE_DIRECTORY ${OUT})
foreach(d ${DATA})
  file(COPY ${SRC}/${d} DESTINATION ${OUT})
endforeach()
set(args)
foreach(f ${FILES})
  list(APPEND args ${SRC}/${f})
endforeach()
execute_process(COMMAND ${MSIM} ${args} -top ${TOP} ${EXTRA}
                WORKING_DIRECTORY ${OUT} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT DEFINED WANT_RC)
  set(WANT_RC 0)
endif()
if(NOT rc EQUAL WANT_RC)
  message(FATAL_ERROR "msim exited ${rc}, not ${WANT_RC}:\n${err}\n${out}")
endif()
# the report location names the file as given on the command line
string(REPLACE "${SRC}/" "" out "${out}")
string(REGEX REPLACE "\\$finish[^\n]*\n" "" out "${out}")
# severity failure: msim's own $fatal lines are not part of the VHDL output
string(REGEX REPLACE "Fatal: [^\n]*\nVHDL (severity|bound check) failure\n" "" out "${out}")
file(READ ${SRC}/${TOP}.out want)
if(NOT out STREQUAL want)
  message(FATAL_ERROR "output differs from ${TOP}.out\n--- got:\n${out}\n--- want:\n${want}")
endif()
