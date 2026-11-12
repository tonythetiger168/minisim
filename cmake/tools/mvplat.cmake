# mvplat: the virtual platform (docs/VPLAT.md). RISC-V harts, a memory map of
# the bundled peripheral models, a JSON platform file. Plain C++20 and
# threads. Its engine headers (src/vp) are installed with the core package,
# so an extension library (vp/ext.hpp) builds against the same engine.
include_guard(GLOBAL)
find_package(Threads REQUIRED)
add_executable(mvplat src/mvplat.cpp)
target_include_directories(mvplat PRIVATE src ${CMAKE_BINARY_DIR}/gen)
target_link_libraries(mvplat PRIVATE Threads::Threads ${CMAKE_DL_LIBS})

find_program(MVPLAT_BASH bash)
if(MVPLAT_BASH)
  # Programs, interrupts, harts on threads, PMP, the trace, platform errors.
  add_test(NAME mvplat
           COMMAND ${MVPLAT_BASH} ${CMAKE_SOURCE_DIR}/tests/mvplat.sh $<TARGET_FILE:mvplat>
                   ${CMAKE_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/mvplat_test)
  # The RISC-V architecture tests; skipped without clang for riscv32 or git.
  add_test(NAME mvplat_isa
           COMMAND ${MVPLAT_BASH} ${CMAKE_SOURCE_DIR}/tests/mvplat_isa.sh $<TARGET_FILE:mvplat>
                   ${CMAKE_CURRENT_BINARY_DIR}/mvplat_isa)
  set_tests_properties(mvplat_isa PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 1200)
endif()

include(GNUInstallDirs)
install(TARGETS mvplat RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES src/vp/bus.hpp src/vp/devices.hpp src/vp/elf.hpp src/vp/ext.hpp src/vp/json.hpp
              src/vp/machine.hpp src/vp/rv32.hpp
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/minisim/vp)
install(FILES docs/VPLAT.md DESTINATION ${CMAKE_INSTALL_DOCDIR})
