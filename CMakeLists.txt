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
foreach(t test_expr test_z test_sched test_net test_sys test_cpu test_upf test_lefdef)
  add_executable(${t} tests/${t}.cpp)
  target_include_directories(${t} PRIVATE src)
  target_compile_options(${t} PRIVATE -UNDEBUG)   # asserts stay on in Release too
  add_test(NAME ${t} COMMAND ${t})
endforeach()
# The Liberty (.lib) reader shared by the timing, synthesis, power and
# equivalence tools (src/liberty.hpp); no external dependencies.
add_executable(test_liberty tests/test_liberty.cpp)
target_include_directories(test_liberty PRIVATE src)
add_test(NAME test_liberty COMMAND test_liberty ${CMAKE_SOURCE_DIR}/tests/sta/sta_test.lib)
# Protected envelopes (IEEE 1800-2023 clause 34 / IEEE 1735) use OpenSSL's
# libcrypto: the msimprotect encryptor, msim -keyfile, and their tests.
find_package(OpenSSL COMPONENTS Crypto)
if(OPENSSL_FOUND)
  add_executable(msimprotect src/msimprotect.cpp)
  target_include_directories(msimprotect PRIVATE src)
  target_link_libraries(msimprotect PRIVATE OpenSSL::Crypto)
  add_executable(test_protect tests/test_protect.cpp)
  target_include_directories(test_protect PRIVATE src)
  target_link_libraries(test_protect PRIVATE OpenSSL::Crypto)
  target_compile_options(test_protect PRIVATE -UNDEBUG)
  add_test(NAME test_protect COMMAND test_protect)
  # Interop: envelopes made by msimprotect open with the openssl command line
  # tool, and an envelope built with the openssl tool opens in msimprotect's
  # decryptor (checked by msim in the slang build below).
  find_program(OPENSSL_CLI openssl)
  if(OPENSSL_CLI)
    add_test(NAME protect_openssl_cli
             COMMAND ${CMAKE_COMMAND} -DOPENSSL=${OPENSSL_CLI} -DPROTECT=$<TARGET_FILE:msimprotect>
                     -DSRC=${CMAKE_SOURCE_DIR} -DOUT=${CMAKE_CURRENT_BINARY_DIR}/protect_openssl
                     -P ${CMAKE_SOURCE_DIR}/tests/protect_openssl.cmake)
  endif()
else()
  message(STATUS "OpenSSL not found: building without protected envelope support")
endif()

# The gate-level Verilog netlist reader and writer (src/gnetlist.hpp).
add_executable(test_gnetlist tests/test_gnetlist.cpp)
target_include_directories(test_gnetlist PRIVATE src)
add_test(NAME test_gnetlist COMMAND test_gnetlist)

add_executable(test_vcd tests/test_vcd.cpp)
add_test(NAME test_vcd COMMAND test_vcd $<TARGET_FILE:minisim>)
# mbug (bug reports) against fake GitHub, Jira and Redmine servers: Python only, no simulator.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
  add_test(NAME mbug
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/mbug_test.py ${CMAKE_SOURCE_DIR}
                   ${CMAKE_CURRENT_BINARY_DIR}/mbug_test)
endif()

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
    # FST_WRITER_PARALLEL: libfst compresses and writes each finished block on
    # a background thread while the simulation goes on (msim turns it on).
    find_package(Threads REQUIRED)
    target_link_libraries(fst PUBLIC ZLIB::ZLIB Threads::Threads)
    target_compile_definitions(fst PRIVATE _GNU_SOURCE HAVE_FSEEKO HAVE_REALPATH
                               HAVE_LIBPTHREAD FST_WRITER_PARALLEL)
    set_target_properties(fst PROPERTIES POSITION_INDEPENDENT_CODE ON)
  endif()

  add_executable(slang_dump src/slang_dump.cpp)
  target_include_directories(slang_dump PRIVATE src)
  target_link_libraries(slang_dump PRIVATE slang::slang)

  # One-step compile + simulate, irun / vcs -R style: msim tb.sv -top tb -vcd w.vcd
  add_executable(msim src/msim.cpp)
  target_include_directories(msim PRIVATE src)
  # DPI-C (35): libffi calls the imports; -rdynamic lets a loaded library
  # find the svdpi.h functions and msim_dpi_export in msim itself.
  find_library(FFI_LIB ffi REQUIRED)
  find_path(FFI_INCLUDE ffi.h PATH_SUFFIXES x86_64-linux-gnu aarch64-linux-gnu)
  target_link_libraries(msim PRIVATE slang::slang ${FFI_LIB} ${CMAKE_DL_LIBS})
  if(FFI_INCLUDE)
    target_include_directories(msim PRIVATE ${FFI_INCLUDE})
  endif()
  set_target_properties(msim PROPERTIES ENABLE_EXPORTS ON)
  if(OPENSSL_FOUND)
    target_link_libraries(msim PRIVATE OpenSSL::Crypto)
    target_compile_definitions(msim PRIVATE MINISIM_HAVE_OPENSSL)
  endif()
  # -compile builds generated C++ against the kernel headers in src/.
  target_compile_definitions(msim PRIVATE MINISIM_SRC_DIR="${CMAKE_SOURCE_DIR}/src")
  if(MINISIM_WITH_FST)
    target_link_libraries(msim PRIVATE fst)
    target_compile_definitions(msim PRIVATE MINISIM_FST MINISIM_FST_PARALLEL MINISIM_FST_DIR="${FST_SRC}")
  endif()

  # Lint and clock-domain checks on the same front end: mlint design.sv
  add_executable(mlint src/mlint.cpp)
  target_include_directories(mlint PRIVATE src)
  target_link_libraries(mlint PRIVATE slang::slang)
  if(OPENSSL_FOUND)
    target_link_libraries(mlint PRIVATE OpenSSL::Crypto)
    target_compile_definitions(mlint PRIVATE MINISIM_HAVE_OPENSSL)
  endif()

  # Design-for-test checks and a scan chain plan: mdft design.sv
  add_executable(mdft src/mdft.cpp)
  target_include_directories(mdft PRIVATE src)
  target_link_libraries(mdft PRIVATE slang::slang)
  if(OPENSSL_FOUND)
    target_link_libraries(mdft PRIVATE OpenSSL::Crypto)
    target_compile_definitions(mdft PRIVATE MINISIM_HAVE_OPENSSL)
  endif()

  # Source-level debug: drivers, loads, active-driver trace, viewer: mdebug design.sv
  add_executable(mdebug src/mdebug.cpp)
  target_include_directories(mdebug PRIVATE src)
  find_package(Threads REQUIRED)
  target_link_libraries(mdebug PRIVATE slang::slang Threads::Threads)

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
  # +notimingcheck (31): the setup and hold checks report nothing.
  add_test(NAME msim_notimingcheck COMMAND msim +notimingcheck ${CMAKE_SOURCE_DIR}/tests/spec/31.3-setup-hold.sv)
  set_tests_properties(msim_notimingcheck PROPERTIES PASS_REGULAR_EXPRESSION "^notifier=x\n$")
  add_test(NAME msim_maxtime COMMAND msim ${CLI}/spin.sv -maxtime 25)
  set_tests_properties(msim_maxtime PROPERTIES PASS_REGULAR_EXPRESSION "^tick\ntick\ntick\n$")
  add_test(NAME msim_vcd
           COMMAND ${CMAKE_COMMAND} -DMSIM=$<TARGET_FILE:msim> -DSRC=${CMAKE_SOURCE_DIR}
                   -P ${CMAKE_SOURCE_DIR}/tests/msim_vcd.cmake)
  add_test(NAME msim_vcdtasks
           COMMAND ${CMAKE_COMMAND} -DMSIM=$<TARGET_FILE:msim> -DSRC=${CMAKE_SOURCE_DIR}
                   -P ${CMAKE_SOURCE_DIR}/tests/msim_vcdtasks.cmake)
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
  # Protected envelopes end to end: msimprotect, then msim -keyfile; no key,
  # the wrong key, redacted diagnostics, and a VCD without protected signals.
  if(OPENSSL_FOUND AND OPENSSL_CLI)
    add_test(NAME msim_protect
             COMMAND ${CMAKE_COMMAND} -DMSIM=$<TARGET_FILE:msim> -DPROTECT=$<TARGET_FILE:msimprotect>
                     -DMLINT=$<TARGET_FILE:mlint> -DMDFT=$<TARGET_FILE:mdft>
                     -DOPENSSL=${OPENSSL_CLI} -DSRC=${CMAKE_SOURCE_DIR}
                     -DOUT=${CMAKE_CURRENT_BINARY_DIR}/msim_protect
                     -P ${CMAKE_SOURCE_DIR}/tests/msim_protect.cmake)
  endif()
  # The virtual prototype layer: the design reaching C models over AXI4-Lite.
  add_test(NAME msim_mvp
           COMMAND ${CMAKE_COMMAND} -DMSIM=$<TARGET_FILE:msim> -DSRC=${CMAKE_SOURCE_DIR}
                   -DOUT=${CMAKE_CURRENT_BINARY_DIR}/msim_mvp
                   -P ${CMAKE_SOURCE_DIR}/tests/mvp.cmake)
  set_tests_properties(msim_mvp PROPERTIES ENVIRONMENT MINISIM_MVP=${CMAKE_SOURCE_DIR}/mvp)

  # DPI-C: -sv_lib, -sv_liblist, -dpiheader, -o, and a missing C function.
  add_test(NAME msim_dpi
           COMMAND ${CMAKE_COMMAND} -DMSIM=$<TARGET_FILE:msim> -DSRC=${CMAKE_SOURCE_DIR}
                   -P ${CMAKE_SOURCE_DIR}/tests/msim_dpi.cmake)
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
    # marker comes off as soon as the gap is fixed (tools/mregress.py -spec tests/spec lists them).
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

  # Datapath library (dw/MW_*.sv, docs/DATAPATH.md): self-checking benches in
  # tests/dw compare every module with the SystemVerilog operators.
  file(GLOB DW_TESTS CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tests/dw/*.sv)
  foreach(sv IN LISTS DW_TESTS)
    get_filename_component(name ${sv} NAME_WE)
    add_test(NAME dw/${name}
             COMMAND ${CMAKE_COMMAND} -DSIM=$<TARGET_FILE:msim> -DSV=${sv}
                     -P ${CMAKE_SOURCE_DIR}/tests/spec/run_spec.cmake)
    set_tests_properties(dw/${name} PROPERTIES LABELS "dw")
  endforeach()

  # UVM (IEEE 1800.2-2020) tests: tests/uvm/*.sv run against the Accellera
  # UVM library when -DUVM_HOME=<uvm-core checkout> is given (CI fetches
  # uvm-core 2020.3.1).
  set(UVM_HOME "$ENV{UVM_HOME}" CACHE PATH "Accellera uvm-core checkout for the UVM tests")
  if(UVM_HOME AND EXISTS ${UVM_HOME}/src/uvm_pkg.sv)
    file(GLOB UVM_TESTS CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tests/uvm/*.sv)
    foreach(sv IN LISTS UVM_TESTS)
      get_filename_component(name ${sv} NAME_WE)
      add_test(NAME uvm/${name}
               COMMAND ${CMAKE_COMMAND} -DMSIM=$<TARGET_FILE:msim> -DUVM_HOME=${UVM_HOME} -DSV=${sv}
                       -P ${CMAKE_SOURCE_DIR}/tests/uvm/run_uvm.cmake)
      set_tests_properties(uvm/${name} PROPERTIES LABELS "uvm")
    endforeach()
  endif()

  add_test(NAME mlint
           COMMAND ${CMAKE_COMMAND} -DMLINT=$<TARGET_FILE:mlint>
                   -DSRC=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/mlint.cmake)
  # Power-aware simulation and UPF checks (IEEE 1801-2024): msim -upf, mlint -upf.
  add_test(NAME upf
           COMMAND ${CMAKE_COMMAND} -DMSIM=$<TARGET_FILE:msim> -DMLINT=$<TARGET_FILE:mlint>
                   -DSRC=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/upf.cmake)

  add_test(NAME mdebug
           COMMAND ${CMAKE_COMMAND} -DMDEBUG=$<TARGET_FILE:mdebug> -DMSIM=$<TARGET_FILE:msim>
                   -DSRC=${CMAKE_SOURCE_DIR} -DOUT=${CMAKE_CURRENT_BINARY_DIR}/mdebug_test
                   -P ${CMAKE_SOURCE_DIR}/tests/mdebug.cmake)
  add_test(NAME mdft
           COMMAND ${CMAKE_COMMAND} -DMDFT=$<TARGET_FILE:mdft>
                   -DSRC=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/mdft.cmake)

  # Boundary scan and IJTAG: generated RTL and testbenches run on msim.
  add_test(NAME mdft_jtag
           COMMAND ${CMAKE_COMMAND} -DMDFT=$<TARGET_FILE:mdft> -DMSIM=$<TARGET_FILE:msim>
                   -DSRC=${CMAKE_SOURCE_DIR} -DOUT=${CMAKE_CURRENT_BINARY_DIR}/mdft_jtag
                   -P ${CMAKE_SOURCE_DIR}/tests/mdft_jtag.cmake)

  add_test(NAME mdft_lbist
           COMMAND ${CMAKE_COMMAND} -DMDFT=$<TARGET_FILE:mdft> -DMSIM=$<TARGET_FILE:msim>
                   -DSRC=${CMAKE_SOURCE_DIR} -DOUT=${CMAKE_CURRENT_BINARY_DIR}/mdft_lbist
                   -P ${CMAKE_SOURCE_DIR}/tests/mdft_lbist.cmake)

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
#   bin/msim, bin/minisim, bin/slang_dump, bin/mregress, bin/mcov, bin/mbug, bin/msimprotect (with OpenSSL)
#   share/minisim/include/   kernel sources that msim -compile builds against
#   include/svdpi.h          the DPI-C header for C code called from SystemVerilog
#   include/vpi_user.h       the VPI header (clauses 36-38)
#   share/doc/minisim/       README, CHANGELOG, LICENSE, COVERAGE
include(GNUInstallDirs)
install(TARGETS minisim RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(MINISIM_WITH_SLANG)
  install(TARGETS msim mlint mdft mdebug slang_dump RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
if(OPENSSL_FOUND)
  install(TARGETS msimprotect RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
install(PROGRAMS tools/mregress.py DESTINATION ${CMAKE_INSTALL_BINDIR} RENAME mregress)
install(PROGRAMS tools/mcov.py DESTINATION ${CMAKE_INSTALL_BINDIR} RENAME mcov)
install(PROGRAMS tools/mbug.py DESTINATION ${CMAKE_INSTALL_BINDIR} RENAME mbug)
install(FILES src/minisim.cpp src/prog.hpp src/expr.hpp src/exec.inc
        DESTINATION ${CMAKE_INSTALL_DATADIR}/minisim/include)
if(MINISIM_WITH_SLANG AND MINISIM_WITH_FST)
  # msim -compile with an FST waveform builds these into simv.
  install(FILES ${FST_SRC}/fstapi.c ${FST_SRC}/fstapi.h ${FST_SRC}/fastlz.c ${FST_SRC}/fastlz.h
                ${FST_SRC}/lz4.c ${FST_SRC}/lz4.h ${FST_SRC}/fst_win_unistd.h ${libfst_SOURCE_DIR}/LICENSE
          DESTINATION ${CMAKE_INSTALL_DATADIR}/minisim/include/fst)
endif()
install(FILES include/svdpi.h include/vpi_user.h include/sv_vpi_user.h include/veriuser.h include/mvp.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
# UVM's C code for minisim: msim -uvm looks for it next to itself.
install(FILES uvm/uvm_dpi_minisim.cc DESTINATION ${CMAKE_INSTALL_DATADIR}/minisim/uvm)
# The virtual prototype layer: msim -mvp looks for these next to itself.
install(FILES mvp/mvp.c mvp/mvp_tlm.h DESTINATION ${CMAKE_INSTALL_DATADIR}/minisim/mvp)
install(FILES mvp/sv/mvp.sv DESTINATION ${CMAKE_INSTALL_DATADIR}/minisim/mvp/sv)
install(DIRECTORY mvp/models DESTINATION ${CMAKE_INSTALL_DATADIR}/minisim/mvp)
# The datapath library: synthesizable SystemVerilog, msim -y <prefix>/share/minisim/dw +libext+.sv
install(DIRECTORY dw DESTINATION ${CMAKE_INSTALL_DATADIR}/minisim FILES_MATCHING PATTERN "*.sv")
install(FILES README.md CHANGELOG.md LICENSE docs/COVERAGE.md docs/VPI.md docs/UVM.md docs/PROTECT.md docs/DATAPATH.md
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
