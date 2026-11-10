# mhls: high-level synthesis, C/C++ to SystemVerilog. No external
# dependencies; the RTL tests need msim (the slang build).
add_executable(mhls src/mhls.cpp)
target_include_directories(mhls PRIVATE src)
find_package(Threads REQUIRED)
target_link_libraries(mhls PRIVATE Threads::Threads)


# Host-compiler check of every design in tests/hls (no simulator needed).
find_package(Python3 COMPONENTS Interpreter)
set(MHLS_PY)
if(Python3_FOUND)
  set(MHLS_PY -DPY=${Python3_EXECUTABLE})
endif()
if(MINISIM_WITH_SLANG)
  # With msim built, the generated RTL is simulated against the same vectors.
  add_test(NAME mhls
           COMMAND ${CMAKE_COMMAND} -DMHLS=$<TARGET_FILE:mhls> -DMSIM=$<TARGET_FILE:msim>
                   -DCXX=${CMAKE_CXX_COMPILER} -DSRC=${CMAKE_SOURCE_DIR}
                   -DOUT=${CMAKE_CURRENT_BINARY_DIR}/mhls_test ${MHLS_PY}
                   -P ${CMAKE_SOURCE_DIR}/tests/mhls.cmake)
else()
  add_test(NAME mhls
           COMMAND ${CMAKE_COMMAND} -DMHLS=$<TARGET_FILE:mhls>
                   -DCXX=${CMAKE_CXX_COMPILER} -DSRC=${CMAKE_SOURCE_DIR}
                   -DOUT=${CMAKE_CURRENT_BINARY_DIR}/mhls_test ${MHLS_PY}
                   -P ${CMAKE_SOURCE_DIR}/tests/mhls.cmake)
endif()
include(GNUInstallDirs)
install(TARGETS mhls RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
