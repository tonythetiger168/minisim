cmake_minimum_required(VERSION 3.20)
project(minisim VERSION 0.1.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Without a build type CMake passes no -O flag, and the kernel runs about 5x
# slower (a 1M-cycle counter: 3.5 s unoptimized, 0.45 s at -O2). Default to
# -O2 but keep assert() on, since tests/test_expr.cpp checks with it.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  add_compile_options(-O2)
endif()

# Version: the project() VERSION above is the release number (docs/RELEASING.md).
# The git commit is added when the source is a git checkout.
set(MINISIM_GIT "")
find_package(Git QUIET)
if(GIT_FOUND AND EXISTS ${CMAKE_SOURCE_DIR}/.git)
  execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
                  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                  OUTPUT_VARIABLE MINISIM_GIT OUTPUT_STRIP_TRAILING_WHITESPACE
                  ERROR_QUIET)
endif()
configure_file(src/version.hpp.in ${CMAKE_BINARY_DIR}/gen/version.hpp @ONLY)
include_directories(${CMAKE_BINARY_DIR}/gen)

# Simulator kernel + hand-built demo design (no external dependencies)
add_executable(minisim src/minisim.cpp)
target_include_directories(minisim PRIVATE src)

# Unit tests: value library, scheduler (test_sched), system tasks (test_sys),
# net initial value (test_net), and the demo VCD's time-0 $dumpvars sample (test_vcd).
enable_testing()
foreach(t test_expr test_z test_sched test_net test_sys)
  add_executable(${t} tests/${t}.cpp)
  target_include_directories(${t} PRIVATE src)
  target_compile_options(${t} PRIVATE -UNDEBUG)   # asserts stay on in Release too
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
  # FST waveforms (msim -vcd x.fst / -fst): GTKWave's libfst (MIT), pinned
  # to a commit. Needs zlib. -DMINISIM_WITH_FST=OFF builds VCD only.
  option(MINISIM_WITH_FST "FST waveform output (fetches libfst)" ON)
  if(MINISIM_WITH_FST)
    enable_language(C)
    find_package(ZLIB REQUIRED)
    FetchContent_Declare(libfst
      GIT_REPOSITORY https://github.com/gtkwave/libfst.git
      GIT_TAG d3cc801ba980dc1bf9c2e00b8ed91556a78ef271)
    FetchContent_Populate(libfst)
    set(FST_SRC ${libfst_SOURCE_DIR}/src)
    add_library(fst STATIC ${FST_SRC}/fstapi.c ${FST_SRC}/fastlz.c ${FST_SRC}/lz4.c)
    target_include_directories(fst PUBLIC ${FST_SRC})
    target_link_libraries(fst PUBLIC ZLIB::ZLIB)
    target_compile_definitions(fst PRIVATE _GNU_SOURCE HAVE_FSEEKO HAVE_REALPATH)
    set_target_properties(fst PROPERTIES POSITION_INDEPENDENT_CODE ON)
  endif()

  add_executable(slang_dump src/slang_dump.cpp)
  target_include_directories(slang_dump PRIVATE src)
  target_link_libraries(slang_dump PRIVATE slang::slang)

  # One-step compile + simulate, irun / vcs -R style: msim tb.sv -top tb -vcd w.vcd
  add_executable(msim src/msim.cpp)
  target_include_directories(msim PRIVATE src)
  target_link_libraries(msim PRIVATE slang::slang)
  # -compile builds generated C++ against the kernel headers in src/.
  target_compile_definitions(msim PRIVATE MINISIM_SRC_DIR="${CMAKE_SOURCE_DIR}/src")
  if(MINISIM_WITH_FST)
    target_link_libraries(msim PRIVATE fst)
    target_compile_definitions(msim PRIVATE MINISIM_FST MINISIM_FST_DIR="${FST_SRC}")
  endif()

  # Lint and clock-domain checks on the same front end: mlint design.sv
  add_executable(mlint src/mlint.cpp)
  target_include_directories(mlint PRIVATE src)
  target_link_libraries(mlint PRIVATE slang::slang)

  # Design-for-test checks and a scan chain plan: mdft design.sv
  add_executable(mdft src/mdft.cpp)
  target_include_directories(mdft PRIVATE src)
  target_link_libraries(mdft PRIVATE slang::slang)

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
  # Compiled mode: generated C++ prints the same as the interpreter.
  add_test(NAME msim_compile
           COMMAND ${CMAKE_COMMAND} -DMSIM=$<TARGET_FILE:msim> -DSRC=${CMAKE_SOURCE_DIR}
                   -DOUT=${CMAKE_CURRENT_BINARY_DIR}/msim_compile
                   -P ${CMAKE_SOURCE_DIR}/tests/msim_compile.cmake)
  # Multi-core: 256 independent lanes run as parallel partitions. Every
  # thread count must print what the single-threaded event-driven run prints.
  add_test(NAME msim_threads
           COMMAND ${CMAKE_COMMAND} -DMSIM=$<TARGET_FILE:msim> -DSRC=${CMAKE_SOURCE_DIR}
                   -P ${CMAKE_SOURCE_DIR}/tests/msim_threads.cmake)
  # 2-state fast path vs 4-state on random expressions.
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_FOUND)
    add_test(NAME msim_fuzz
             COMMAND ${CMAKE_COMMAND} -DMSIM=$<TARGET_FILE:msim> -DSRC=${CMAKE_SOURCE_DIR}
                     -DOUT=${CMAKE_CURRENT_BINARY_DIR} -DPY=${Python3_EXECUTABLE}
                     -P ${CMAKE_SOURCE_DIR}/tests/msim_fuzz.cmake)
  endif()
  # mregress: test lists, seeds, pass/fail checks, reports, -rerun. With
  # Icarus installed it also runs the spec suite through the iverilog backend.
  if(Python3_FOUND)
    add_test(NAME mregress_smoke
             COMMAND ${CMAKE_COMMAND} -DPY=${Python3_EXECUTABLE} -DMSIM=$<TARGET_FILE:msim>
                     -DSRC=${CMAKE_SOURCE_DIR} -DOUT=${CMAKE_CURRENT_BINARY_DIR}/mregress_smoke
                     -P ${CMAKE_SOURCE_DIR}/tests/mregress.cmake)
  endif()
  if(Python3_FOUND)
    add_test(NAME msim_cov
             COMMAND ${CMAKE_COMMAND} -DPY=${Python3_EXECUTABLE} -DMSIM=$<TARGET_FILE:msim>
                     -DSRC=${CMAKE_SOURCE_DIR} -DOUT=${CMAKE_CURRENT_BINARY_DIR}/msim_cov
                     -P ${CMAKE_SOURCE_DIR}/tests/msim_cov.cmake)
  endif()
  if(MINISIM_WITH_FST)
    add_executable(test_fst tests/test_fst.cpp)
    target_link_libraries(test_fst PRIVATE fst)
    add_test(NAME msim_fst
             COMMAND ${CMAKE_COMMAND} -DMSIM=$<TARGET_FILE:msim> -DCHK=$<TARGET_FILE:test_fst>
                     -DSRC=${CMAKE_SOURCE_DIR} -DOUT=${CMAKE_CURRENT_BINARY_DIR}/msim_fst
                     -P ${CMAKE_SOURCE_DIR}/tests/msim_fst.cmake)
  endif()
  add_test(NAME msim_missing_file COMMAND msim ${CLI}/no_such_file.sv)
  set_tests_properties(msim_missing_file PROPERTIES WILL_FAIL TRUE)

  # Spec suite: tests/spec/<clause>-<topic>.sv, one IEEE 1800-2023 clause each,
  # expected output in "//> " comments. With Icarus Verilog installed, the
  # same expectations are also checked against it (icarus/<name>), unless the
  # file has a "//! icarus-skip: <reason>" line.
  file(GLOB SPEC_TESTS CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tests/spec/*.sv)
  find_program(IVERILOG iverilog)
  find_program(VVP vvp)
  foreach(sv IN LISTS SPEC_TESTS)
    get_filename_component(name ${sv} NAME)
    string(REGEX REPLACE "\\.sv$" "" name ${name})
    add_test(NAME spec/${name}
             COMMAND ${CMAKE_COMMAND} -DSIM=$<TARGET_FILE:msim> -DSV=${sv}
                     -P ${CMAKE_SOURCE_DIR}/tests/spec/run_spec.cmake)
    # A known gap says so on a line "//! xfail: <what is missing>". The test
    # still runs; it is expected to fail, and passing it is an error, so the
    # marker comes off as soon as the gap is fixed (tests/regress.py lists them).
    file(STRINGS ${sv} xfail REGEX "^//! xfail")
    if(xfail)
      set_tests_properties(spec/${name} PROPERTIES WILL_FAIL TRUE LABELS "spec;xfail")
    else()
      set_tests_properties(spec/${name} PROPERTIES LABELS "spec")
    endif()
    # A test Icarus cannot run says why on a line "//! icarus-skip: <reason>".
    file(STRINGS ${sv} skip REGEX "^//! icarus-skip")
    if(IVERILOG AND VVP AND NOT skip)
      add_test(NAME icarus/${name}
               COMMAND ${CMAKE_COMMAND} -DSIM=${IVERILOG} -DVVP=${VVP} -DSV=${sv}
                       -P ${CMAKE_SOURCE_DIR}/tests/spec/run_spec.cmake)
    endif()
  endforeach()

  add_test(NAME mlint
           COMMAND ${CMAKE_COMMAND} -DMLINT=$<TARGET_FILE:mlint>
                   -DSRC=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/mlint.cmake)

  add_test(NAME mdft
           COMMAND ${CMAKE_COMMAND} -DMDFT=$<TARGET_FILE:mdft>
                   -DSRC=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/mdft.cmake)

  if(Python3_FOUND AND IVERILOG AND VVP)
    add_test(NAME mregress_spec_icarus
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/mregress.py -spec ${CMAKE_SOURCE_DIR}/tests/spec
                     -sim iverilog -exe ${IVERILOG} -q -o ${CMAKE_CURRENT_BINARY_DIR}/mregress_icarus)
  endif()

  add_test(NAME slang_dump_counter
           COMMAND slang_dump ${CMAKE_SOURCE_DIR}/examples/counter.sv)
  set_tests_properties(slang_dump_counter PROPERTIES
    PASS_REGULAR_EXPRESSION "clk;rst;cnt")

  add_test(NAME slang_dump_hier
           COMMAND slang_dump ${CMAKE_SOURCE_DIR}/examples/hier.sv)
  set_tests_properties(slang_dump_hier PROPERTIES
    PASS_REGULAR_EXPRESSION "module child")
endif()

# Install layout (cmake --install, and the cpack packages below):
#   bin/msim, bin/minisim, bin/slang_dump
#   share/minisim/include/   kernel sources that msim -compile builds against
#   share/doc/minisim/       README, CHANGELOG, LICENSE, COVERAGE
include(GNUInstallDirs)
install(TARGETS minisim RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(MINISIM_WITH_SLANG)
  install(TARGETS msim mlint mdft slang_dump RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
install(PROGRAMS tools/mregress.py DESTINATION ${CMAKE_INSTALL_BINDIR} RENAME mregress)
install(PROGRAMS tools/mcov.py DESTINATION ${CMAKE_INSTALL_BINDIR} RENAME mcov)
install(FILES src/minisim.cpp src/prog.hpp src/expr.hpp src/exec.inc
        DESTINATION ${CMAKE_INSTALL_DATADIR}/minisim/include)
if(MINISIM_WITH_SLANG AND MINISIM_WITH_FST)
  # msim -compile with an FST waveform builds these into simv.
  install(FILES ${FST_SRC}/fstapi.c ${FST_SRC}/fstapi.h ${FST_SRC}/fastlz.c ${FST_SRC}/fastlz.h
                ${FST_SRC}/lz4.c ${FST_SRC}/lz4.h ${FST_SRC}/fst_win_unistd.h ${libfst_SOURCE_DIR}/LICENSE
          DESTINATION ${CMAKE_INSTALL_DATADIR}/minisim/include/fst)
endif()
install(FILES README.md CHANGELOG.md LICENSE docs/COVERAGE.md
        DESTINATION ${CMAKE_INSTALL_DOCDIR})

set(CPACK_PACKAGE_NAME minisim)
set(CPACK_PACKAGE_VENDOR "minisim")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "SystemVerilog simulator (IEEE 1800-2023) with an irun/vcs-style command line")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/tonythetiger168/minisim")
set(CPACK_RESOURCE_FILE_LICENSE ${CMAKE_SOURCE_DIR}/LICENSE)
set(CPACK_PACKAGE_CONTACT "https://github.com/tonythetiger168/minisim/issues")
set(CPACK_GENERATOR TGZ)
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
# msim -compile runs a C++20 compiler on the generated design.
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "g++ (>= 11)")
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
include(CPack)
