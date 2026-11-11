# mpopt: RTL power optimization, clock gating, memory gating and operand
# isolation (docs/POWEROPT.md). Needs the slang front end.
include_guard(GLOBAL)
if(MINISIM_WITH_SLANG)
  find_package(Threads REQUIRED)
  add_executable(mpopt src/mpopt.cpp)
  target_include_directories(mpopt PRIVATE src)
  target_link_libraries(mpopt PRIVATE slang::slang Threads::Threads)

  # Found, rewritten, co-simulated against the original (uses msim).
  add_test(NAME mpopt
           COMMAND ${CMAKE_COMMAND} -DMPOPT=$<TARGET_FILE:mpopt> -DSRC=${CMAKE_SOURCE_DIR}
                   -DMEQUIV=$<TARGET_FILE_DIR:mpopt>/mequiv -DMPOWER=$<TARGET_FILE_DIR:mpopt>/mpower
                   -DOUT=${CMAKE_CURRENT_BINARY_DIR}/mpopt_test -P ${CMAKE_SOURCE_DIR}/tests/mpopt.cmake)
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_FOUND)
    # Random RTL through mpopt -verify: every rewrite must simulate like its original.
    add_test(NAME mpopt_fuzz
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/popt/fuzz_popt.py $<TARGET_FILE:mpopt>
                     ${CMAKE_CURRENT_BINARY_DIR}/mpopt_fuzz 40)
  endif()

  include(GNUInstallDirs)
  install(TARGETS mpopt RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
