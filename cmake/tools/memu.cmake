# memu: a gate-level netlist onto one or more FPGAs for emulation and
# prototyping (docs/EMULATION.md). Plain C++20 and threads: no slang, so it
# builds in every configuration.
include_guard(GLOBAL)
find_package(Threads REQUIRED)
add_executable(memu src/memu.cpp)
target_include_directories(memu PRIVATE src)
target_compile_definitions(memu PRIVATE MINISIM_SRC_DIR="${CMAKE_SOURCE_DIR}/src")
target_link_libraries(memu PRIVATE Threads::Threads)

# Netlists on FPGAs against the netlists themselves, and the transactors
# against plain simulation. That needs msim, which exists with slang; without
# it the test runs memu alone.
if(MINISIM_WITH_SLANG)
  set(memu_msim -DMSIM=$<TARGET_FILE:msim>)
else()
  set(memu_msim)
endif()
add_test(NAME memu
         COMMAND ${CMAKE_COMMAND} -DMEMU=$<TARGET_FILE:memu> ${memu_msim} -DSRC=${CMAKE_SOURCE_DIR}
                 -DOUT=${CMAKE_CURRENT_BINARY_DIR}/memu_test -P ${CMAKE_SOURCE_DIR}/tests/memu.cmake)

include(GNUInstallDirs)
install(TARGETS memu RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
# The transactors: memu -xtor-sv looks for these next to itself.
install(FILES emu/sv/memu_xtor.sv emu/sv/memu_mvp_host.sv DESTINATION ${CMAKE_INSTALL_DATADIR}/minisim/emu/sv)
