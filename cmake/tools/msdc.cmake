# The Tcl interpreter (src/tcl.hpp), the SDC reader (src/sdc.hpp) and the
# constraint checker msdc (src/msdc.cpp).
include_guard(GLOBAL)
find_package(Threads REQUIRED)
foreach(t test_sdc test_tcl)
  add_executable(${t} tests/${t}.cpp)
  target_include_directories(${t} PRIVATE src)
  target_compile_options(${t} PRIVATE -UNDEBUG)   # asserts stay on in Release too
  target_link_libraries(${t} PRIVATE Threads::Threads)
  add_test(NAME ${t} COMMAND ${t})
endforeach()

if(MINISIM_WITH_SLANG)
  # msdc design.sv -top top -sdc top.sdc
  add_executable(msdc src/msdc.cpp)
  target_include_directories(msdc PRIVATE src)
  target_link_libraries(msdc PRIVATE slang::slang Threads::Threads)
  add_test(NAME msdc
           COMMAND ${CMAKE_COMMAND} -DMSDC=$<TARGET_FILE:msdc> -DSRC=${CMAKE_SOURCE_DIR}
                   -DOUT=${CMAKE_CURRENT_BINARY_DIR} -P ${CMAKE_SOURCE_DIR}/tests/msdc.cmake)
  install(TARGETS msdc RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
