# msim -xprop (docs/XPROP.md): each tests/xprop/*.sv prints its "//> " lines
# in every execution mode: event-driven on one thread, the default
# interpreter, four threads, and msim -compile. "//! args:" gives the test's
# options (-xprop, -xprop=xmerge). Also checks -xprop-report.
# Called with -DMSIM=<msim> -DSRC=<source dir> -DOUT=<scratch dir>.
file(GLOB tests ${SRC}/tests/xprop/*.sv)
list(SORT tests)
file(MAKE_DIRECTORY ${OUT})
set(MODES
  "-threads 1 -no-static-comb -no-static-seq -no-partition"
  ""
  "-threads 4"
  "-compile -O1")
foreach(sv IN LISTS tests)
  get_filename_component(dir ${sv} DIRECTORY)
  get_filename_component(base ${sv} NAME)
  file(STRINGS ${sv} lines)
  set(expected "")
  set(extra "")
  foreach(l IN LISTS lines)
    if(l MATCHES "^//> (.*)$")
      string(APPEND expected "${CMAKE_MATCH_1}\n")
    elseif(l STREQUAL "//>")
      string(APPEND expected "\n")
    elseif(l MATCHES "^//! args: (.*)$")
      separate_arguments(extra UNIX_COMMAND "${CMAKE_MATCH_1}")
    endif()
  endforeach()
  foreach(m IN LISTS MODES)
    separate_arguments(mode UNIX_COMMAND "${m}")
    if(m MATCHES "-compile")
      list(APPEND mode -o ${OUT}/${base}.d)
    endif()
    execute_process(COMMAND ${MSIM} ${base} ${extra} ${mode} OUTPUT_VARIABLE got ERROR_VARIABLE err
                    RESULT_VARIABLE rc TIMEOUT 300 WORKING_DIRECTORY ${dir})
    if(NOT rc EQUAL 0)
      message(FATAL_ERROR "msim ${base} ${extra} ${m} exited ${rc}:\n${err}")
    endif()
    if(NOT got STREQUAL expected)
      message(FATAL_ERROR "msim ${base} ${extra} ${m}: output differs\n--- expected\n${expected}--- got\n${got}")
    endif()
  endforeach()
endforeach()
# The report names what is merged and what is not, with the reason.
execute_process(COMMAND ${MSIM} checks-fallback.sv -xprop -xprop-report ${OUT}/report.txt
                OUTPUT_QUIET RESULT_VARIABLE rc WORKING_DIRECTORY ${SRC}/tests/xprop)
file(READ ${OUT}/report.txt rep)
if(NOT rc EQUAL 0 OR NOT rep MATCHES "xprop tmerge: 13 of 16" OR NOT rep MATCHES "case: merged"
   OR NOT rep MATCHES "if: not merged, it holds a jump out of the branch"
   OR NOT rep MATCHES "loop body: merged"
   OR NOT rep MATCHES "if: not merged, it holds a task or function call statement"
   OR NOT rep MATCHES "edge: merged")
  message(FATAL_ERROR "unexpected -xprop-report:\n${rep}")
endif()
execute_process(COMMAND ${MSIM} off.sv -xprop=nonsense RESULT_VARIABLE rc OUTPUT_QUIET ERROR_QUIET
                WORKING_DIRECTORY ${SRC}/tests/xprop)
if(NOT rc EQUAL 2)
  message(FATAL_ERROR "msim -xprop=nonsense exited ${rc}, expected 2")
endif()
message(STATUS "xprop: every test matches in every mode")
