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

# A signal and a process added to one module (linux x86-64: the parts name
# signals by link-time symbols, codegen.hpp relocText): design.hpp and the
# parts of the other modules stay the same; only the part of the edited
# process and the part of the new one change.
cmake_host_system_information(RESULT os QUERY OS_NAME)
cmake_host_system_information(RESULT cpu QUERY OS_PLATFORM)
if(os STREQUAL "Linux" AND cpu MATCHES "x86_64|AMD64")
  file(REMOVE_RECURSE "${OUT}/c" "${OUT}/d")
  foreach(v c d)
    set(def "")
    if(v STREQUAL "d")
      set(def "+define+SIG")
    endif()
    execute_process(COMMAND ${CMAKE_COMMAND} -E env MSIM_NOKOBJ=1
                    "${MSIM}" -compile -cfiles 4 -cc true -o "${OUT}/${v}" ${def} "${SRC}/examples/partsig.sv"
                    OUTPUT_QUIET ERROR_QUIET)
    if(NOT EXISTS "${OUT}/${v}/part3.cpp")
      message(FATAL_ERROR "msim -compile -cfiles 4 did not write 4 parts (partsig)")
    endif()
  endforeach()
  set(changed 0)
  foreach(f design.hpp part0.cpp part1.cpp part2.cpp part3.cpp)
    file(READ "${OUT}/c/${f}" ta)
    file(READ "${OUT}/d/${f}" tb)
    if(NOT ta STREQUAL tb)
      if(f STREQUAL "design.hpp")
        message(FATAL_ERROR "design.hpp changed with a signal added")
      endif()
      math(EXPR changed "${changed} + 1")
    endif()
  endforeach()
  if(changed EQUAL 0 OR changed GREATER 2)
    message(FATAL_ERROR "${changed} of 4 parts changed with a signal and a process added (want 1 or 2)")
  endif()
  message(STATUS "${changed} of 4 parts changed with a signal added")
endif()
