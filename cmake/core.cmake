# minisim::core - the headers a tool outside this repository builds against.
#
# The tools share a kernel: the lowering pass, the expression and instruction
# representation, the interpreter, the lint IR, the waveform and activity
# readers, the thread pool. A tool kept in another repository needs exactly
# these headers, so they are installed and exported as a CMake package rather
# than copied. See docs/LICENSING.md for which tools live where.
#
#   find_package(minisimCore REQUIRED)
#   target_link_libraries(mytool PRIVATE minisim::core)
#
# The package is header-only apart from libfst, which comes with it when
# minisim was built with FST support.

# Kept in step with tests/core_consumer/: that test builds a tool against the
# installed package, so a header that is used but not listed here fails the
# build rather than being found by accident through the source tree.
set(MINISIM_CORE_HEADERS
    annexd.inc
    clocking.hpp
    compat.hpp
    cover.hpp
    cover_rt.inc
    cpu.hpp
    exec.inc
    expr.hpp
    fsm_extract.hpp
    gnetlist.hpp
    interval.hpp
    liberty.hpp
    licproxy.hpp
    lintir.hpp
    lower.hpp
    lower_assert.inc
    lower_clocking.inc
    lower_cover.inc
    lower_xprop.inc
    parallel.hpp
    prog.hpp
    profile.hpp
    protect.hpp
    protect_front.hpp
    rand.inc
    saif.hpp
    sdf.hpp
    sdf_annotate.inc
    specify.hpp
    tcl.hpp
    sva.hpp
    sva_engine.hpp
    tchk.hpp
    tchk.inc
    tcl.hpp
    upf.hpp
    upf_impl.inc
    upf_vct.hpp
    upf_query.hpp
    upf_rt.inc
    vpi.inc
    vpi_code.inc
    vpi_covmerge.inc
    vpi_data.inc
    vpi_prim.inc
    vpi_constr.inc
    vpi_svcb.inc
    vpi_thread.inc
    vpi_tchk.inc
    wave_reader.hpp)

add_library(minisim_core INTERFACE)
add_library(minisim::core ALIAS minisim_core)
# The alias above is the build tree's name; EXPORT_NAME is what a consumer of
# the installed package sees, and the two have to agree.
set_target_properties(minisim_core PROPERTIES EXPORT_NAME core)
target_compile_features(minisim_core INTERFACE cxx_std_20)
target_include_directories(minisim_core INTERFACE
                           $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src>
                           $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/minisim>)
# slang is fetched rather than installed, so it cannot go into the export set.
# The package config links it in the consumer's build instead, where the
# consumer has either installed slang or fetched the same version.
target_link_libraries(minisim_core INTERFACE $<BUILD_INTERFACE:slang::slang> Threads::Threads)
if(OPENSSL_FOUND)
  # protect.hpp decrypts IEEE 1735 envelopes; without OpenSSL it reports that
  # the build has no crypto rather than failing to compile. licproxy.hpp talks
  # TLS to an IP author's license proxy (IEEE 1735 clause 8), hence SSL.
  target_link_libraries(minisim_core INTERFACE OpenSSL::SSL OpenSSL::Crypto)
  target_compile_definitions(minisim_core INTERFACE MINISIM_HAVE_OPENSSL)
endif()

set(MINISIM_CORE_EXPORTS minisim_core)
if(MINISIM_WITH_FST)
  # wave_reader.hpp reads FST through libfst, so a consumer needs it too.
  target_link_libraries(minisim_core INTERFACE fst)
  target_compile_definitions(minisim_core INTERFACE MINISIM_FST)
  target_include_directories(fst INTERFACE
                             $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/minisim>)
  list(APPEND MINISIM_CORE_EXPORTS fst)
  # fst_win_unistd.h is what fstapi.h reaches for on Windows. It is never
  # compiled on a Unix host, but a consumer that builds the package on Windows
  # needs it, and shipping a header whose own include is missing is a trap
  # either way.
  install(FILES ${FST_SRC}/fstapi.h ${FST_SRC}/fst_win_unistd.h
          DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/minisim)
endif()

list(TRANSFORM MINISIM_CORE_HEADERS PREPEND ${CMAKE_SOURCE_DIR}/src/
     OUTPUT_VARIABLE MINISIM_CORE_HEADER_PATHS)
install(FILES ${MINISIM_CORE_HEADER_PATHS} ${CMAKE_BINARY_DIR}/gen/version.hpp
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/minisim)
install(TARGETS ${MINISIM_CORE_EXPORTS} EXPORT minisimCoreTargets)
install(EXPORT minisimCoreTargets NAMESPACE minisim::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/minisimCore)

# Recorded in the package config so a consumer that lacks slang is told which
# version to fetch instead of meeting an unresolved slang symbol.
set(MINISIM_CORE_SLANG_TAG v12.1)

include(CMakePackageConfigHelpers)
write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/minisimCoreConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMinorVersion)
configure_package_config_file(
  ${CMAKE_SOURCE_DIR}/cmake/minisimCoreConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/minisimCoreConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/minisimCore)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/minisimCoreConfig.cmake
              ${CMAKE_CURRENT_BINARY_DIR}/minisimCoreConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/minisimCore)
