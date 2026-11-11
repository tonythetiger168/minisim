# mpss: portable stimulus (Accellera PSS). Solves scenarios from a model and
# writes each as a UVM sequence and as a C test (docs/MPSS.md).
#   mpss model.pss -n 20 -sv seqs.sv -c tests.c
# Plain C++20 and threads: no slang, so it builds in every configuration.
include_guard(GLOBAL)
find_package(Threads REQUIRED)
add_executable(mpss src/mpss.cpp)
target_include_directories(mpss PRIVATE src)
target_link_libraries(mpss PRIVATE Threads::Threads)

# The test builds and runs the generated C when a C compiler is found.
find_program(MPSS_CC NAMES cc gcc clang)
add_test(NAME mpss
         COMMAND ${CMAKE_COMMAND} -DMPSS=$<TARGET_FILE:mpss> -DSRC=${CMAKE_SOURCE_DIR}
                 -DOUT=${CMAKE_CURRENT_BINARY_DIR}/mpss_test -DCC=${MPSS_CC}
                 -P ${CMAKE_SOURCE_DIR}/tests/mpss.cmake)

# With the slang build and UVM (-DUVM_HOME or $UVM_HOME, as the uvm/ tests):
# the generated sequences under msim -uvm print what the C tests print.
set(MPSS_UVM_HOME "${UVM_HOME}")
if(NOT MPSS_UVM_HOME)
  set(MPSS_UVM_HOME "$ENV{UVM_HOME}")
endif()
if(MINISIM_WITH_SLANG AND MPSS_UVM_HOME AND EXISTS ${MPSS_UVM_HOME}/src/uvm_pkg.sv)
  add_test(NAME mpss_uvm
           COMMAND ${CMAKE_COMMAND} -DMPSS=$<TARGET_FILE:mpss> -DSRC=${CMAKE_SOURCE_DIR}
                   -DOUT=${CMAKE_CURRENT_BINARY_DIR}/mpss_uvm_test -DCC=${MPSS_CC}
                   -DMSIM=$<TARGET_FILE:msim> -DUVM_HOME=${MPSS_UVM_HOME}
                   -P ${CMAKE_SOURCE_DIR}/tests/mpss.cmake)
  set_tests_properties(mpss_uvm PROPERTIES LABELS "uvm")
endif()

include(GNUInstallDirs)
install(TARGETS mpss RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES docs/MPSS.md DESTINATION ${CMAKE_INSTALL_DOCDIR})
