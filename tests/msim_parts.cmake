# msim -compile rebuilds only the generated C++ files whose text changed.
# An edit to one module must change only the files holding that module's
# code: here at most 2 of 4 parts (mixer's 2-state and 4-state copies), and
# neither design.hpp nor design.cpp. The C++ compiler is not run (-cc true).
# Called with -DMSIM=<msim> -DSRC=<source dir> -DOUT=<scratch dir>.
file(REMOVE_RECURSE "${OUT}/a" "${OUT}/b")
foreach(v a b)
  set(def "")
  if(v STREQUAL "b")
    set(def "+define+EDIT")
  endif()
  execute_process(COMMAND ${CMAKE_COMMAND} -E env MSIM_NOKOBJ=1
                  "${MSIM}" -compile -cfiles 4 -cc true -o "${OUT}/${v}" ${def} "${SRC}/examples/partedit.sv"
                  OUTPUT_QUIET ERROR_QUIET)
  if(NOT EXISTS "${OUT}/${v}/part3.cpp")
    message(FATAL_ERROR "msim -compile -cfiles 4 did not write 4 parts")
  endif()
endforeach()
set(changed 0)
foreach(f design.hpp design.cpp part0.cpp part1.cpp part2.cpp part3.cpp)
  file(READ "${OUT}/a/${f}" ta)
  file(READ "${OUT}/b/${f}" tb)
  if(NOT ta STREQUAL tb)
    if(f MATCHES "^design")
      message(FATAL_ERROR "${f} changed with an edit to one module")
    endif()
    math(EXPR changed "${changed} + 1")
  endif()
endforeach()
if(changed EQUAL 0 OR changed GREATER 2)
  message(FATAL_ERROR "${changed} of 4 parts changed with an edit to one module (want 1 or 2)")
endif()
message(STATUS "${changed} of 4 parts changed")
