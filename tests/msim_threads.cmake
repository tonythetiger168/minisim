# msim -threads N prints the same as the single-threaded event-driven run,
# for every N, on a design with many independent partitions (bench/wide.sv).
# Called with -DMSIM=<msim> -DSRC=<source dir>.
set(design ${SRC}/bench/wide.sv +define+END_TIME=5000)
execute_process(COMMAND ${MSIM} -threads 1 -no-static-comb -no-static-seq -no-partition ${design}
                OUTPUT_VARIABLE want RESULT_VARIABLE r)
if(NOT r EQUAL 0 OR NOT want MATCHES "checksum=")
  message(FATAL_ERROR "reference run failed (${r}): ${want}")
endif()
foreach(t 1 2 3 4)
  execute_process(COMMAND ${MSIM} -threads ${t} ${design} OUTPUT_VARIABLE got RESULT_VARIABLE r ERROR_QUIET)
  if(NOT r EQUAL 0 OR NOT want STREQUAL got)
    message(FATAL_ERROR "-threads ${t} differs\n--- reference\n${want}--- got\n${got}")
  endif()
endforeach()
execute_process(COMMAND ${MSIM} ${design} OUTPUT_VARIABLE got RESULT_VARIABLE r)
if(NOT want STREQUAL got)
  message(FATAL_ERROR "default thread count differs\n--- reference\n${want}--- got\n${got}")
endif()
message(STATUS "all thread counts match: ${want}")
