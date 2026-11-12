# msim -cm_cond std / full / allops on tests/cov/cond_modes.sv: the vectors of
# each mode, and the interpreter and -compile writing the same conditions.
#   cmake -DPY=<python3> -DMSIM=<msim> -DSRC=<source dir> -DOUT=<dir> -P msim_cond_modes.cmake
file(REMOVE_RECURSE ${OUT})
file(MAKE_DIRECTORY ${OUT})
set(SV ${SRC}/tests/cov/cond_modes.sv)
set(MCOV ${PY} ${SRC}/tools/mcov.py)
set(want_std "Condition: 75\\.0% \\(3/4 vectors of 1 condition"
             "     1   1   1   1\n     0   1   1   1\n     1   0   1   Not covered\n     1   1   0   1")
set(want_full "Condition: 62\\.5% \\(5/8 vectors of 1 condition"
              "     0   0   0   1\n     0   0   1   Not covered\n     0   1   0   Not covered\n     0   1   1   1\n     1   0   0   1\n     1   0   1   Not covered\n     1   1   0   1\n     1   1   1   1")
set(want_allops "Condition: 90\\.0% \\(9/10 vectors of 3 condition"
                "cond_modes\\.sv:6:19  \\(a & b\\) \\| c\n[^\n]*\n[^\n]*\n[^\n]*\n     0   0   2\n     1   0   1\n     0   1   1"
                "cond_modes\\.sv:6:20  \\(a & b\\)\n[^\n]*\n[^\n]*\n[^\n]*\n     1   1   2\n     0   1   1\n     1   0   1")
set(want_both "Condition: 81\\.2% \\(13/16 vectors of 3 condition"
              "cond_modes\\.sv:6:20  \\(a & b\\)\n[^\n]*\n[^\n]*\n[^\n]*\n     0   0   1\n     0   1   1\n     1   0   1\n     1   1   2")
foreach(mode std full allops "full+allops")
  set(key ${mode})
  if(mode STREQUAL "full+allops")
    set(key both)
  endif()
  set(hows interp)
  if(key STREQUAL both)   # -compile once, with both modes: each compile takes a while
    list(APPEND hows compile)
  endif()
  foreach(how ${hows})
    set(extra)
    if(how STREQUAL compile)
      set(extra -compile)
    endif()
    execute_process(COMMAND ${MSIM} ${SV} ${extra} -cm_cond ${mode} -cov ${key}_${how}.json
                    WORKING_DIRECTORY ${OUT} RESULT_VARIABLE rc OUTPUT_QUIET ERROR_VARIABLE err)
    if(NOT rc EQUAL 0)
      message(FATAL_ERROR "msim ${extra} -cm_cond ${mode} exited ${rc}: ${err}")
    endif()
    execute_process(COMMAND ${MCOV} report ${key}_${how}.json -detail WORKING_DIRECTORY ${OUT} OUTPUT_VARIABLE out)
    foreach(want ${want_${key}})
      if(NOT out MATCHES "${want}")
        message(FATAL_ERROR "-cm_cond ${mode} (${how}): no match for '${want}' in\n${out}")
      endif()
    endforeach()
  endforeach()
endforeach()
execute_process(COMMAND ${MSIM} ${SV} -cm_cond nope -cov x.json WORKING_DIRECTORY ${OUT} RESULT_VARIABLE rc OUTPUT_QUIET ERROR_VARIABLE err)
if(rc EQUAL 0 OR NOT err MATCHES "unknown mode 'nope'")
  message(FATAL_ERROR "an unknown -cm_cond mode should be an error, got ${rc}: ${err}")
endif()
message(STATUS "condition coverage modes: ok")
