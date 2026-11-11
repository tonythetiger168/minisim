# The exported kernel package: install minisim into a scratch prefix, then
# build tests/core_consumer against it the way a tool in another repository
# does. A kernel header that minisimCore forgets to install, or a missing link
# dependency, fails here instead of in that other repository.
#   cmake -DBUILD=<build dir> -DSRC=<source dir> -DOUT=<dir> -DSLANG_*=... -P core_lib.cmake
file(REMOVE_RECURSE ${OUT})
file(MAKE_DIRECTORY ${OUT})
set(prefix ${OUT}/prefix)

execute_process(COMMAND ${CMAKE_COMMAND} --install ${BUILD} --prefix ${prefix}
                --component Unspecified
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "installing into the scratch prefix failed: ${rc}\n${out}${err}")
endif()

# Everything the package promises must be there before a consumer is tried.
foreach(want lib/cmake/minisimCore/minisimCoreConfig.cmake
             lib/cmake/minisimCore/minisimCoreTargets.cmake
             include/minisim/lintir.hpp include/minisim/prog.hpp
             include/minisim/parallel.hpp include/minisim/lower.hpp
             include/minisim/version.hpp)
  if(NOT EXISTS ${prefix}/${want})
    message(FATAL_ERROR "the minisimCore package did not install ${want}")
  endif()
endforeach()

# An exported header that quotes a sibling the package does not install leaves
# the consumer with a missing-header error, which is how interval.hpp was found
# missing while rand.inc shipped. Every quoted sibling must be installed too.
file(GLOB installed ${prefix}/include/minisim/*)
foreach(header ${installed})
  file(STRINGS ${header} includes REGEX "^[ \t]*#[ \t]*include[ \t]*\"")
  foreach(line ${includes})
    string(REGEX REPLACE "^.*#[ \t]*include[ \t]*\"([^\"]+)\".*$" "\\1" sibling "${line}")
    get_filename_component(name ${sibling} NAME)
    if(NOT sibling MATCHES "/" AND NOT EXISTS ${prefix}/include/minisim/${name})
      get_filename_component(from ${header} NAME)
      message(FATAL_ERROR
              "${from} is installed but includes \"${name}\", which the "
              "minisimCore package does not install: add it to "
              "MINISIM_CORE_HEADERS in cmake/core.cmake.")
    endif()
  endforeach()
endforeach()

# The driver joins slang's include list with | so it survives as one argument.
string(REPLACE "|" ";" SLANG_INCLUDE_DIRS "${SLANG_INCLUDE_DIRS}")

execute_process(COMMAND ${CMAKE_COMMAND} -S ${SRC}/tests/core_consumer -B ${OUT}/build
                        -DCMAKE_BUILD_TYPE=Release
                        -DCMAKE_PREFIX_PATH=${prefix}
                        -DSLANG_LIBRARY=${SLANG_LIBRARY}
                        "-DSLANG_INCLUDE_DIRS=${SLANG_INCLUDE_DIRS}"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "configuring the consumer against the package failed: ${rc}\n${out}${err}")
endif()

execute_process(COMMAND ${CMAKE_COMMAND} --build ${OUT}/build --parallel
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "building the consumer against the package failed: ${rc}\n${out}${err}")
endif()

execute_process(COMMAND ${OUT}/build/core_consumer
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "the consumer ran but failed: ${rc}\n${out}${err}")
endif()
if(NOT out MATCHES "core_consumer: minisim [0-9]+\\.[0-9]+\\.[0-9]+")
  message(FATAL_ERROR "the consumer did not report the version it was built against:\n${out}")
endif()

message(STATUS "core_lib: ok - ${out}")
