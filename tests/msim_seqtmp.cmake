# Blocking temporaries of a clocked block (msim.cpp: localizeSeqBlocking):
# run-time index writes into them, and the $urandom stimulus of the
# processes started after the block, are the same as in the event-driven
# reference, at every thread count and with them off (MSIM_NOSEQTMP=1).
# Called with -DMSIM=<msim> -DSRC=<source dir>.
set(design ${SRC}/examples/seqtmpidx.sv)
execute_process(COMMAND ${MSIM} -threads 1 -no-static-comb -no-static-seq -no-partition ${design}
                OUTPUT_VARIABLE want RESULT_VARIABLE r)
if(NOT r EQUAL 0 OR NOT want MATCHES "a0=[0-9a-f]+ a1=[0-9a-f]+ din=[0-9a-f]+ r=")
  message(FATAL_ERROR "reference run failed (${r}): ${want}")
endif()
foreach(mode "-threads 1" "-threads 4" "" "NOSEQTMP")
  if(mode STREQUAL "NOSEQTMP")
    execute_process(COMMAND ${CMAKE_COMMAND} -E env MSIM_NOSEQTMP=1 ${MSIM} ${design} OUTPUT_VARIABLE got RESULT_VARIABLE r)
  else()
    separate_arguments(a UNIX_COMMAND "${mode}")
    execute_process(COMMAND ${MSIM} ${a} ${design} OUTPUT_VARIABLE got RESULT_VARIABLE r)
  endif()
  if(NOT r EQUAL 0 OR NOT want STREQUAL got)
    message(FATAL_ERROR "'${mode}' differs\n--- reference\n${want}--- got\n${got}")
  endif()
endforeach()
message(STATUS "seqtmp matches the reference: ${want}")
