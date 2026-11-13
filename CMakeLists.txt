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
# net initial value (test_net), the demo VCD's time-0 $dumpvars sample (test_vcd),
# and the Tcl interpreter of the tool line (test_tcl).
enable_testing()
foreach(t test_expr test_z test_sched test_net test_sys test_cpu test_upf test_tcl)
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
# libcrypto. What is here is the reading half: msim -keyfile opens an envelope
# and src/protect.hpp is tested directly. The encryptor that makes one,
# msimprotect, is a paid tool (docs/LICENSING.md).
# The license system (IEEE 1735 clause 8) talks TLS to the IP author's
# license proxy, so msim links libssl too.
find_package(OpenSSL COMPONENTS Crypto SSL)
if(OPENSSL_FOUND)
  add_executable(test_protect tests/test_protect.cpp)
  target_include_directories(test_protect PRIVATE src)
  target_link_libraries(test_protect PRIVATE OpenSSL::SSL OpenSSL::Crypto)
  target_compile_options(test_protect PRIVATE -UNDEBUG)
  add_test(NAME test_protect COMMAND test_protect)
else()
  message(STATUS "OpenSSL not found: building without protected envelope support")
endif()

# The gate-level Verilog netlist reader and writer (src/gnetlist.hpp).
add_executable(test_gnetlist tests/test_gnetlist.cpp)
target_include_directories(test_gnetlist PRIVATE src)
add_test(NAME test_gnetlist COMMAND test_gnetlist)
# The SAIF reader (src/saif.hpp), state- and path-dependent counts included.
find_package(Threads REQUIRED)
add_executable(test_saif tests/test_saif.cpp)
target_include_directories(test_saif PRIVATE src)
target_compile_options(test_saif PRIVATE -UNDEBUG)
target_link_libraries(test_saif PRIVATE Threads::Threads)
add_test(NAME test_saif COMMAND test_saif ${CMAKE_SOURCE_DIR}/tests/power/gates_sdpd.saif)

add_executable(test_vcd tests/test_vcd.cpp)
add_test(NAME test_vcd COMMAND test_vcd $<TARGET_FILE:minisim>)
# mbug (bug reports) against fake GitHub, Jira and Redmine servers: Python only, no simulator.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
  add_test(NAME mbug
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/mbug_test.py ${CMAKE_SOURCE_DIR}
                   ${CMAKE_CURRENT_BINARY_DIR}/mbug_test)
endif()

# Shared unit tests that need nothing but the compiler: one file each in
# cmake/tools.
include(cmake/tools/shared_par_wave.cmake)

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
    target_include_directories(fst PUBLIC $<BUILD_INTERFACE:${FST_SRC}>)
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
    target_link_libraries(msim PRIVATE OpenSSL::SSL OpenSSL::Crypto)
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
    target_link_libraries(mlint PRIVATE OpenSSL::SSL OpenSSL::Crypto)
    target_compile_definitions(mlint PRIVATE MINISIM_HAVE_OPENSSL)
  endif()

  # Design-for-test checks and a scan chain plan: mdft design.sv
  add_executable(mdft src/mdft.cpp)
  target_include_directories(mdft PRIVATE src)
  find_package(Threads REQUIRED)
  target_link_libraries(mdft PRIVATE slang::slang Threads::Threads)
  if(OPENSSL_FOUND)
    target_link_libraries(mdft PRIVATE OpenSSL::SSL OpenSSL::Crypto)
    target_compile_definitions(mdft PRIVATE MINISIM_HAVE_OPENSSL)
  endif()

  # Source-level debug: drivers, loads, active-driver trace, viewer: mdebug design.sv
  add_executable(mdebug src/mdebug.cpp)
  target_include_directories(mdebug PRIVATE src)
  find_package(Threads REQUIRED)
  target_link_libraries(mdebug PRIVATE slang::slang Threads::Threads)
  if(MINISIM_WITH_FST)
    target_link_libraries(mdebug PRIVATE fst)
    target_compile_definitions(mdebug PRIVATE MINISIM_FST)
  endif()

  add_executable(test_lower tests/test_lower.cpp)
  target_include_directories(test_lower PRIVATE src)
  target_link_libraries(test_lower PRIVATE slang::slang)
  target_compile_definitions(test_lower PRIVATE MINISIM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
  add_test(NAME test_lower COMMAND test_lower)

  # The FSM extractor as other tools use it: the header and slang, nothing else.
  add_executable(test_fsm_extract tests/test_fsm_extract.cpp)
  target_include_directories(test_fsm_extract PRIVATE src)
  target_link_libraries(test_fsm_extract PRIVATE slang::slang)
  target_compile_definitions(test_fsm_extract PRIVATE MINISIM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
  add_test(NAME test_fsm_extract COMMAND test_fsm_extract)

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
  # X-propagation (msim -xprop): tests/xprop in every execution mode.
  add_test(NAME msim_xprop
           COMMAND ${CMAKE_COMMAND} -DMSIM=$<TARGET_FILE:msim> -DSRC=${CMAKE_SOURCE_DIR}
                   -DOUT=${CMAKE_CURRENT_BINARY_DIR}/msim_xprop
                   -P ${CMAKE_SOURCE_DIR}/tests/msim_xprop.cmake)
  # Interactive simulation (msim -i, -do FILE): run, step, stop, force, print.
  add_test(NAME msim_interactive
           COMMAND ${CMAKE_COMMAND} -DMSIM=$<TARGET_FILE:msim> -DSRC=${CMAKE_SOURCE_DIR}
                   -P ${CMAKE_SOURCE_DIR}/tests/msim_interactive.cmake)
  # The UCLI commands of msim -i: line stops, step/next, Tcl, scopes, dump.
  add_test(NAME msim_ucli
           COMMAND ${CMAKE_COMMAND} -DMSIM=$<TARGET_FILE:msim> -DSRC=${CMAKE_SOURCE_DIR}
                   -DOUT=${CMAKE_CURRENT_BINARY_DIR}/msim_ucli -P ${CMAKE_SOURCE_DIR}/tests/msim_ucli.cmake)
  # mlint findings in mdebug's page (mdebug -lint).
  add_test(NAME mdebug_lint
           COMMAND ${CMAKE_COMMAND} -DMDEBUG=$<TARGET_FILE:mdebug> -DMLINT=$<TARGET_FILE:mlint> -DSRC=${CMAKE_SOURCE_DIR}
                   -DOUT=${CMAKE_CURRENT_BINARY_DIR}/mdebug_lint -P ${CMAKE_SOURCE_DIR}/tests/mdebug_lint.cmake)
  # Class objects in the waveform (msim -vcd-class), read back by mdebug.
  add_test(NAME msim_vcd_class
           COMMAND ${CMAKE_COMMAND} -DMSIM=$<TARGET_FILE:msim> -DMDEBUG=$<TARGET_FILE:mdebug> -DSRC=${CMAKE_SOURCE_DIR}
                   -DOUT=${CMAKE_CURRENT_BINARY_DIR}/msim_vcd_class
                   -P ${CMAKE_SOURCE_DIR}/tests/msim_vcd_class.cmake)
  # Checkpoints: $save, then msim -restore (like simv -r).
  add_test(NAME msim_restore
           COMMAND ${CMAKE_COMMAND} -DMSIM=$<TARGET_FILE:msim> -DSRC=${CMAKE_SOURCE_DIR}
                   -DOUT=${CMAKE_CURRENT_BINARY_DIR}/msim_restore
                   -P ${CMAKE_SOURCE_DIR}/tests/msim_restore.cmake)
  # The xrun / irun command line (msim run as xrun, or msim -xrun).
  add_test(NAME msim_xrun
           COMMAND ${CMAKE_COMMAND} -DMSIM=$<TARGET_FILE:msim> -DSRC=${CMAKE_SOURCE_DIR}
                   -DOUT=${CMAKE_CURRENT_BINARY_DIR}/msim_xrun
                   -P ${CMAKE_SOURCE_DIR}/tests/msim_xrun.cmake)
  # Simulation profile (msim -profile): processes, instances, kinds.
  add_test(NAME msim_profile
           COMMAND ${CMAKE_COMMAND} -DMSIM=$<TARGET_FILE:msim> -DSRC=${CMAKE_SOURCE_DIR}
                   -DOUT=${CMAKE_CURRENT_BINARY_DIR}/msim_profile
                   -P ${CMAKE_SOURCE_DIR}/tests/msim_profile.cmake)
  # Multi-core: 256 independent lanes run as parallel partitions. Every
  # thread count must print what the single-threaded event-driven run prints.
  add_test(NAME msim_threads
           COMMAND ${CMAKE_COMMAND} -DMSIM=$<TARGET_FILE:msim> -DSRC=${CMAKE_SOURCE_DIR}
                   -P ${CMAKE_SOURCE_DIR}/tests/msim_threads.cmake)
  # 2-state fast path vs 4-state on random expressions.
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_FOUND)
    # Compiled mode with a waveform: the same values as the interpreter's VCD.
    add_test(NAME msim_compile_vcd
             COMMAND ${CMAKE_COMMAND} -DMSIM=$<TARGET_FILE:msim> -DSRC=${CMAKE_SOURCE_DIR}
                     -DOUT=${CMAKE_CURRENT_BINARY_DIR}/msim_compile_vcd -DPY=${Python3_EXECUTABLE}
                     -P ${CMAKE_SOURCE_DIR}/tests/msim_compile_vcd.cmake)
    # mdebug on about 6000 signals: on-demand waveform reading, VCD = FST.
    add_test(NAME mdebug_large
             COMMAND ${CMAKE_COMMAND} -DMSIM=$<TARGET_FILE:msim> -DMDEBUG=$<TARGET_FILE:mdebug> -DSRC=${CMAKE_SOURCE_DIR}
                     -DOUT=${CMAKE_CURRENT_BINARY_DIR}/mdebug_large -DPY=${Python3_EXECUTABLE} -DFST=$<BOOL:${MINISIM_WITH_FST}>
                     -P ${CMAKE_SOURCE_DIR}/tests/mdebug_large.cmake)
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
    # -farm slurm on a real one-node Slurm (CI starts it); skips without one.
    add_test(NAME mregress_slurm
             COMMAND ${CMAKE_COMMAND} -DPY=${Python3_EXECUTABLE} -DMSIM=$<TARGET_FILE:msim>
                     -DSRC=${CMAKE_SOURCE_DIR} -DOUT=${CMAKE_CURRENT_BINARY_DIR}/mregress_slurm
                     -P ${CMAKE_SOURCE_DIR}/tests/mregress_slurm.cmake)
  endif()
  # mplan: a verification plan scored against a regression (tests/plan).
  if(Python3_FOUND)
    add_test(NAME mplan
             COMMAND ${CMAKE_COMMAND} -DPY=${Python3_EXECUTABLE} -DMSIM=$<TARGET_FILE:msim>
                     -DSRC=${CMAKE_SOURCE_DIR} -DOUT=${CMAKE_CURRENT_BINARY_DIR}/mplan_test
                     -P ${CMAKE_SOURCE_DIR}/tests/mplan.cmake)
  endif()
  if(Python3_FOUND)
    add_test(NAME msim_cov
             COMMAND ${CMAKE_COMMAND} -DPY=${Python3_EXECUTABLE} -DMSIM=$<TARGET_FILE:msim>
                     -DSRC=${CMAKE_SOURCE_DIR} -DOUT=${CMAKE_CURRENT_BINARY_DIR}/msim_cov
                     -P ${CMAKE_SOURCE_DIR}/tests/msim_cov.cmake)
    add_test(NAME msim_fsm
             COMMAND ${CMAKE_COMMAND} -DPY=${Python3_EXECUTABLE} -DMSIM=$<TARGET_FILE:msim>
                     -DSRC=${CMAKE_SOURCE_DIR} -DOUT=${CMAKE_CURRENT_BINARY_DIR}/msim_fsm
                     -P ${CMAKE_SOURCE_DIR}/tests/msim_fsm.cmake)
    add_test(NAME mcov_excl
             COMMAND ${CMAKE_COMMAND} -DPY=${Python3_EXECUTABLE} -DMSIM=$<TARGET_FILE:msim>
                     -DSRC=${CMAKE_SOURCE_DIR} -DOUT=${CMAKE_CURRENT_BINARY_DIR}/mcov_excl
                     -P ${CMAKE_SOURCE_DIR}/tests/mcov_excl.cmake)
    add_test(NAME mcov_ucis
             COMMAND ${CMAKE_COMMAND} -DPY=${Python3_EXECUTABLE} -DMSIM=$<TARGET_FILE:msim>
                     -DSRC=${CMAKE_SOURCE_DIR} -DOUT=${CMAKE_CURRENT_BINARY_DIR}/mcov_ucis
                     -P ${CMAKE_SOURCE_DIR}/tests/mcov_ucis.cmake)
    add_test(NAME mcov_compat
             COMMAND ${CMAKE_COMMAND} -DPY=${Python3_EXECUTABLE} -DMSIM=$<TARGET_FILE:msim>
                     -DSRC=${CMAKE_SOURCE_DIR} -DOUT=${CMAKE_CURRENT_BINARY_DIR}/mcov_compat
                     -P ${CMAKE_SOURCE_DIR}/tests/mcov_compat.cmake)
    add_test(NAME msim_cov_select
             COMMAND ${CMAKE_COMMAND} -DPY=${Python3_EXECUTABLE} -DMSIM=$<TARGET_FILE:msim>
                     -DSRC=${CMAKE_SOURCE_DIR} -DOUT=${CMAKE_CURRENT_BINARY_DIR}/msim_cov_select
                     -P ${CMAKE_SOURCE_DIR}/tests/msim_cov_select.cmake)
    add_test(NAME msim_cov_inst
             COMMAND ${CMAKE_COMMAND} -DPY=${Python3_EXECUTABLE} -DMSIM=$<TARGET_FILE:msim>
                     -DSRC=${CMAKE_SOURCE_DIR} -DOUT=${CMAKE_CURRENT_BINARY_DIR}/msim_cov_inst
                     -P ${CMAKE_SOURCE_DIR}/tests/msim_cov_inst.cmake)
    add_test(NAME msim_cond_modes
             COMMAND ${CMAKE_COMMAND} -DPY=${Python3_EXECUTABLE} -DMSIM=$<TARGET_FILE:msim>
                     -DSRC=${CMAKE_SOURCE_DIR} -DOUT=${CMAKE_CURRENT_BINARY_DIR}/msim_cond_modes
                     -P ${CMAKE_SOURCE_DIR}/tests/msim_cond_modes.cmake)
  endif()
  if(MINISIM_WITH_FST)
    add_executable(test_fst tests/test_fst.cpp)
    target_link_libraries(test_fst PRIVATE fst)
    add_test(NAME msim_fst
             COMMAND ${CMAKE_COMMAND} -DMSIM=$<TARGET_FILE:msim> -DCHK=$<TARGET_FILE:test_fst>
                     -DSRC=${CMAKE_SOURCE_DIR} -DOUT=${CMAKE_CURRENT_BINARY_DIR}/msim_fst
                     -P ${CMAKE_SOURCE_DIR}/tests/msim_fst.cmake)
  endif()
  # DPI-C: -sv_lib, -sv_liblist, -dpiheader, -o, and a missing C function.
  add_test(NAME msim_dpi
           COMMAND ${CMAKE_COMMAND} -DMSIM=$<TARGET_FILE:msim> -DSRC=${CMAKE_SOURCE_DIR}
                   -P ${CMAKE_SOURCE_DIR}/tests/msim_dpi.cmake)
  # VPI and DPI cannot look into protected IP (37.3.6; IEEE 1735 9.7). The
  # check needs an envelope, so it needs the encryptor, which is a paid tool:
  # it runs beside msimprotect instead of here, with the license proxy test
  # (clause 8) and the rule-by-rule checks of 5.2, 9.3, 9.4, 9.9 and Annex A.
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
      # Both runs work in tests/spec: a test that writes a file there
      # ($fopen, $writememh) must not run under both simulators at once.
      set_tests_properties(spec/${name} icarus/${name} PROPERTIES RESOURCE_LOCK spec_${name})
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
    # UVM transaction recording read back by mdebug -tr.
    add_test(NAME mdebug_uvm_tr
             COMMAND ${CMAKE_COMMAND} -DMSIM=$<TARGET_FILE:msim> -DMDEBUG=$<TARGET_FILE:mdebug> -DUVM_HOME=${UVM_HOME}
                     -DSRC=${CMAKE_SOURCE_DIR} -DOUT=${CMAKE_CURRENT_BINARY_DIR}/mdebug_uvm_tr
                     -P ${CMAKE_SOURCE_DIR}/tests/mdebug_uvm_tr.cmake)
    set_tests_properties(mdebug_uvm_tr PROPERTIES LABELS "uvm")
  endif()

  # mini45's Verilog models against the functions in its Liberty files.
  add_test(NAME mini45_models
           COMMAND msim ${CMAKE_SOURCE_DIR}/lib/mini45/mini45.v ${CMAKE_SOURCE_DIR}/lib/mini45/mini45_tb.sv
                   -top mini45_tb)
  set_tests_properties(mini45_models PROPERTIES PASS_REGULAR_EXPRESSION "mini45: 84 cells checked, 0 errors")

  add_test(NAME mlint
           COMMAND ${CMAKE_COMMAND} -DMLINT=$<TARGET_FILE:mlint>
                   -DSRC=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/mlint.cmake)
  # mlint -cdc-sva writes assertions that msim runs next to the design.
  add_test(NAME mlint_cdc_sva
           COMMAND ${CMAKE_COMMAND} -DMLINT=$<TARGET_FILE:mlint> -DMSIM=$<TARGET_FILE:msim>
                   -DSRC=${CMAKE_SOURCE_DIR} -DOUT=${CMAKE_CURRENT_BINARY_DIR}/mlint_cdc_sva
                   -P ${CMAKE_SOURCE_DIR}/tests/mlint_cdc_sva.cmake)
  # Every mlint rule on a marked source file (tests/lint/rules), the
  # configuration, the waivers, and -threads 1 against -threads 4.
  if(Python3_FOUND)
    add_test(NAME mlint_rules
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/lint/check_rules.py
                     $<TARGET_FILE:mlint> ${CMAKE_SOURCE_DIR}/tests/lint)
  endif()
  # Power-aware simulation and UPF checks (IEEE 1801-2024): msim -upf, mlint -upf.
  add_test(NAME upf
           COMMAND ${CMAKE_COMMAND} -DMSIM=$<TARGET_FILE:msim> -DMLINT=$<TARGET_FILE:mlint>
                   -DSRC=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tests/upf.cmake)
  # Forward SAIF (IEEE 1801-2024 Annex J): msim -saif -saif_forward.
  add_test(NAME saif_forward
           COMMAND ${CMAKE_COMMAND} -DMSIM=$<TARGET_FILE:msim> -DSRC=${CMAKE_SOURCE_DIR}
                   -P ${CMAKE_SOURCE_DIR}/tests/saif_fwd.cmake)

  add_test(NAME mdebug
           COMMAND ${CMAKE_COMMAND} -DMDEBUG=$<TARGET_FILE:mdebug> -DMSIM=$<TARGET_FILE:msim>
                   -DSRC=${CMAKE_SOURCE_DIR} -DOUT=${CMAKE_CURRENT_BINARY_DIR}/mdebug_test
                   -DFST=${MINISIM_WITH_FST} -DPY=${Python3_EXECUTABLE}
                   -P ${CMAKE_SOURCE_DIR}/tests/mdebug.cmake)
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
#   bin/msim, bin/minisim, bin/slang_dump, bin/mregress, bin/mcov, bin/mbug, bin/mplan
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
endif()
# minisim::core: the kernel headers, exported so a tool kept in another
# repository builds against them instead of copying them (docs/LICENSING.md).
if(MINISIM_WITH_SLANG)
  include(cmake/core.cmake)

  # A tool outside this repository, built against the installed package: the
  # test installs minisim into a scratch prefix and builds tests/core_consumer
  # there, so a header the package forgets to install fails here.
  add_test(NAME core_lib
           COMMAND ${CMAKE_COMMAND} -DBUILD=${CMAKE_BINARY_DIR} -DSRC=${CMAKE_SOURCE_DIR}
                   -DOUT=${CMAKE_CURRENT_BINARY_DIR}/core_lib
                   -DSLANG_LIBRARY=$<TARGET_FILE:slang::slang>
                   # A list would be split into separate arguments on its
                   # semicolons, so it is joined and split again in the script.
                   # slang's own link interface is left out: it names targets
                   # (Boost, fmt, tomlplusplus) that are header-only and exist
                   # only in this build.
                   "-DSLANG_INCLUDE_DIRS=$<JOIN:$<TARGET_PROPERTY:slang::slang,INTERFACE_INCLUDE_DIRECTORIES>,|>"
                   -P ${CMAKE_SOURCE_DIR}/tests/core_lib.cmake)
  set_tests_properties(core_lib PROPERTIES TIMEOUT 900)
endif()
install(PROGRAMS tools/mregress.py DESTINATION ${CMAKE_INSTALL_BINDIR} RENAME mregress)
install(PROGRAMS tools/mcov.py DESTINATION ${CMAKE_INSTALL_BINDIR} RENAME mcov)
install(PROGRAMS tools/mbug.py DESTINATION ${CMAKE_INSTALL_BINDIR} RENAME mbug)
install(PROGRAMS tools/mplan.py DESTINATION ${CMAKE_INSTALL_BINDIR} RENAME mplan)
install(PROGRAMS tools/mplan_serve.py DESTINATION ${CMAKE_INSTALL_BINDIR} RENAME mplan_serve)
# The Python interface to mdebug -script (import mdebug_api).
install(FILES tools/mdebug_api.py DESTINATION ${CMAKE_INSTALL_DATADIR}/minisim/python)
install(FILES src/minisim.cpp src/prog.hpp src/expr.hpp src/exec.inc
              src/upf_rt.inc src/upf.hpp src/upf_impl.inc src/upf_vct.hpp src/tcl.hpp   # msim -compile -upf
        DESTINATION ${CMAKE_INSTALL_DATADIR}/minisim/include)
if(MINISIM_WITH_SLANG AND MINISIM_WITH_FST)
  # msim -compile with an FST waveform builds these into simv.
  install(FILES ${FST_SRC}/fstapi.c ${FST_SRC}/fstapi.h ${FST_SRC}/fastlz.c ${FST_SRC}/fastlz.h
                ${FST_SRC}/lz4.c ${FST_SRC}/lz4.h ${FST_SRC}/fst_win_unistd.h ${libfst_SOURCE_DIR}/LICENSE
          DESTINATION ${CMAKE_INSTALL_DATADIR}/minisim/include/fst)
endif()
install(FILES include/svdpi.h include/vpi_user.h include/vpi_compatibility.h include/sv_vpi_user.h include/veriuser.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
# UVM's C code for minisim: msim -uvm looks for it next to itself.
install(FILES uvm/uvm_dpi_minisim.cc DESTINATION ${CMAKE_INSTALL_DATADIR}/minisim/uvm)
# The datapath library: synthesizable SystemVerilog, msim -y <prefix>/share/minisim/dw +libext+.sv
install(DIRECTORY dw DESTINATION ${CMAKE_INSTALL_DATADIR}/minisim FILES_MATCHING PATTERN "*.sv")
install(FILES README.md CHANGELOG.md LICENSE NOTICE THIRD_PARTY_NOTICES.md docs/LICENSING.md docs/COVERAGE.md docs/VPI.md docs/UVM.md docs/PROTECT.md docs/DATAPATH.md docs/MPLAN.md
        DESTINATION ${CMAKE_INSTALL_DOCDIR})

# The tools: one file each in cmake/tools, all included here, after the core
# targets, the slang front end, FST and the install directories, so a tool
# file can test if(MINISIM_WITH_SLANG), if(TARGET msim) and MINISIM_WITH_FST.
# Add a file, not a line: every *.cmake there is included. A file that is
# also named in an include() line above is skipped here, so a missing
# include_guard(GLOBAL) cannot define its targets twice.
file(GLOB TOOL_CMAKE CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/cmake/tools/*.cmake)
file(READ ${CMAKE_CURRENT_LIST_FILE} TOOL_CMAKE_SELF)
foreach(f ${TOOL_CMAKE})
  get_filename_component(n ${f} NAME)
  string(FIND "${TOOL_CMAKE_SELF}" "include(cmake/tools/${n})" explicit)
  if(explicit EQUAL -1)
    include(${f})
  endif()
endforeach()

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
