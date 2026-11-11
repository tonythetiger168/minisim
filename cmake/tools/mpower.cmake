# mpower: power analysis (average, peak, RTL estimate, UPF domains).
#   mpower design.v -top dut -lib cells.lib -activity run.saif -strip_path tb.dut
# Needs the slang front end, msim (for its test) and the FST option; the top
# CMakeLists.txt includes every cmake/tools/*.cmake after those are set up.
include_guard(GLOBAL)
if(NOT TARGET msim)
  return()
endif()
include(GNUInstallDirs)
add_executable(mpower ${CMAKE_SOURCE_DIR}/src/mpower.cpp)
target_include_directories(mpower PRIVATE ${CMAKE_SOURCE_DIR}/src)
find_package(Threads REQUIRED)
target_link_libraries(mpower PRIVATE slang::slang Threads::Threads)
if(MINISIM_WITH_FST)
  target_link_libraries(mpower PRIVATE fst)
  target_compile_definitions(mpower PRIVATE MINISIM_FST)
endif()
install(TARGETS mpower RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# Hand-checked numbers on a three-cell netlist, every activity format, the RTL
# estimate, UPF domains, peak power, and the same answer on any thread count.
add_test(NAME mpower
         COMMAND ${CMAKE_COMMAND} -DMPOWER=$<TARGET_FILE:mpower> -DMSIM=$<TARGET_FILE:msim>
                 -DSRC=${CMAKE_SOURCE_DIR} -DOUT=${CMAKE_CURRENT_BINARY_DIR}/mpower_test
                 -DFST=${MINISIM_WITH_FST}
                 -P ${CMAKE_SOURCE_DIR}/tests/mpower.cmake)
