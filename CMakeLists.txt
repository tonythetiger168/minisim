cmake_minimum_required(VERSION 3.20)
project(minisim LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Simulator kernel + hand-built demo design (no external dependencies)
add_executable(minisim src/minisim.cpp)
target_include_directories(minisim PRIVATE src)

# Unit tests: value library, scheduler (test_sched), system tasks (test_sys),
# net initial value (test_net), and the demo VCD's time-0 $dumpvars sample (test_vcd).
enable_testing()
foreach(t test_expr test_z test_sched test_net test_sys)
  add_executable(${t} tests/${t}.cpp)
  target_include_directories(${t} PRIVATE src)
  add_test(NAME ${t} COMMAND ${t})
endforeach()
add_executable(test_vcd tests/test_vcd.cpp)
add_test(NAME test_vcd COMMAND test_vcd $<TARGET_FILE:minisim>)

# Optional slang front end. slang v12.1 is the release this lowerer compiles
# against. Mimalloc's -mno-outline is a clang flag that g++ rejects, so the
# default is off; -DSLANG_USE_MIMALLOC=ON still overrides it.
option(MINISIM_WITH_SLANG "Build the slang front end and lowering tests" OFF)
if(MINISIM_WITH_SLANG)
  set(SLANG_USE_MIMALLOC OFF CACHE BOOL "g++ rejects mimalloc's -mno-outline")
  include(FetchContent)
  FetchContent_Declare(slang
    GIT_REPOSITORY https://github.com/MikePopoloski/slang.git
    GIT_TAG v12.1
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(slang)
  add_executable(slang_dump src/slang_dump.cpp)
  target_include_directories(slang_dump PRIVATE src)
  target_link_libraries(slang_dump PRIVATE slang::slang)

  # One-step compile + simulate, irun / vcs -R style: msim tb.sv -top tb -vcd w.vcd
  add_executable(msim src/msim.cpp)
  target_include_directories(msim PRIVATE src)
  target_link_libraries(msim PRIVATE slang::slang)

  add_executable(test_lower tests/test_lower.cpp)
  target_include_directories(test_lower PRIVATE src)
  target_link_libraries(test_lower PRIVATE slang::slang)
  target_compile_definitions(test_lower PRIVATE MINISIM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
  add_test(NAME test_lower COMMAND test_lower)

  # msim end to end: two files, -top, +define+, -maxtime, -vcd, and a bad file.
  set(CLI ${CMAKE_SOURCE_DIR}/examples/cli)
  add_test(NAME msim_define
           COMMAND msim ${CLI}/adder.sv ${CLI}/adder_tb.sv -top adder_tb +define+WIDTH=8)
  set_tests_properties(msim_define PROPERTIES PASS_REGULAR_EXPRESSION "W= +8 sum= *44\n")
  add_test(NAME msim_default_width
           COMMAND msim -sv ${CLI}/adder.sv ${CLI}/adder_tb.sv -top adder_tb)
  set_tests_properties(msim_default_width PROPERTIES PASS_REGULAR_EXPRESSION "W= +4 sum=12\n")
  add_test(NAME msim_init COMMAND msim ${CLI}/init.sv)
  set_tests_properties(msim_init PROPERTIES PASS_REGULAR_EXPRESSION "v= +5\nw= +6\nw= +10\n")
  add_test(NAME msim_maxtime COMMAND msim ${CLI}/spin.sv -maxtime 25)
  set_tests_properties(msim_maxtime PROPERTIES PASS_REGULAR_EXPRESSION "^tick\ntick\ntick\n$")
  add_test(NAME msim_vcd
           COMMAND ${CMAKE_COMMAND} -DMSIM=$<TARGET_FILE:msim> -DSRC=${CMAKE_SOURCE_DIR}
                   -P ${CMAKE_SOURCE_DIR}/tests/msim_vcd.cmake)
  add_test(NAME msim_missing_file COMMAND msim ${CLI}/no_such_file.sv)
  set_tests_properties(msim_missing_file PROPERTIES WILL_FAIL TRUE)

  # Spec suite: tests/spec/<clause>-<topic>.sv, one IEEE 1800-2023 clause each,
  # expected output in "//> " comments. With Icarus Verilog installed, the
  # same expectations are also checked against it (icarus/<name>).
  file(GLOB SPEC_TESTS CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tests/spec/*.sv)
  find_program(IVERILOG iverilog)
  find_program(VVP vvp)
  foreach(sv IN LISTS SPEC_TESTS)
    get_filename_component(name ${sv} NAME)
    string(REGEX REPLACE "\\.sv$" "" name ${name})
    add_test(NAME spec/${name}
             COMMAND ${CMAKE_COMMAND} -DSIM=$<TARGET_FILE:msim> -DSV=${sv}
                     -P ${CMAKE_SOURCE_DIR}/tests/spec/run_spec.cmake)
    if(IVERILOG AND VVP)
      add_test(NAME icarus/${name}
               COMMAND ${CMAKE_COMMAND} -DSIM=${IVERILOG} -DVVP=${VVP} -DSV=${sv}
                       -P ${CMAKE_SOURCE_DIR}/tests/spec/run_spec.cmake)
    endif()
  endforeach()

  add_test(NAME slang_dump_counter
           COMMAND slang_dump ${CMAKE_SOURCE_DIR}/examples/counter.sv)
  set_tests_properties(slang_dump_counter PROPERTIES
    PASS_REGULAR_EXPRESSION "clk;rst;cnt")

  add_test(NAME slang_dump_hier
           COMMAND slang_dump ${CMAKE_SOURCE_DIR}/examples/hier.sv)
  set_tests_properties(slang_dump_hier PROPERTIES
    PASS_REGULAR_EXPRESSION "module child")
endif()
