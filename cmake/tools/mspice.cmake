# mspice: the SPICE circuit simulator. No dependencies beyond the C++ library.
include_guard(GLOBAL)
add_executable(mspice src/mspice.cpp)
target_include_directories(mspice PRIVATE src)
find_package(Threads REQUIRED)
target_link_libraries(mspice PRIVATE Threads::Threads)

# BSIM4 (.model level 14/54): the BSIM group's model code (Educational
# Community License 2.0, see THIRD_PARTY_NOTICES.md), unmodified in
# third_party/bsim4, behind mspice's adapter. A program that uses spice.hpp
# gets BSIM4 by linking mspice_bsim4.
option(MINISIM_WITH_BSIM4 "Build mspice with the BSIM4 transistor model" ON)
if(MINISIM_WITH_BSIM4)
  enable_language(C)
  file(GLOB BSIM4_SOURCES ${CMAKE_SOURCE_DIR}/third_party/bsim4/*.c)
  add_library(mspice_bsim4 STATIC ${BSIM4_SOURCES} src/spice_bsim4.cpp)
  target_include_directories(mspice_bsim4 PRIVATE src src/spice_bsim4 third_party/bsim4)
  target_compile_definitions(mspice_bsim4 INTERFACE MSPICE_BSIM4)
  set_source_files_properties(${BSIM4_SOURCES} PROPERTIES COMPILE_OPTIONS "-w")
  set_target_properties(mspice_bsim4 PROPERTIES C_STANDARD 11 C_EXTENSIONS ON)
  target_link_libraries(mspice_bsim4 PRIVATE Threads::Threads)
  target_link_libraries(mspice PRIVATE mspice_bsim4)
  install(FILES THIRD_PARTY_NOTICES.md DESTINATION ${CMAKE_INSTALL_DOCDIR})
  install(FILES third_party/bsim4/LICENSE DESTINATION ${CMAKE_INSTALL_DOCDIR} RENAME LICENSE.bsim4)
endif()
install(TARGETS mspice RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES docs/SPICE.md DESTINATION ${CMAKE_INSTALL_DOCDIR})

# Each tests/spice/*.cir carries its expected .measure values (worked out by
# hand) in "*>" comments; tests/spice/check.py checks them, checks that the
# output is identical at 1, 3 and all threads, and, when ngspice is installed,
# checks every measurement against ngspice too.
if(Python3_FOUND)
  find_program(NGSPICE ngspice)
  file(GLOB SPICE_TESTS ${CMAKE_SOURCE_DIR}/tests/spice/*.cir)
  foreach(f ${SPICE_TESTS})
    get_filename_component(name ${f} NAME_WE)
    if(name MATCHES "^bsim4" AND NOT MINISIM_WITH_BSIM4)
      continue()
    endif()
    add_test(NAME spice/${name}
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/spice/check.py $<TARGET_FILE:mspice> ${f}
                     $<$<BOOL:${NGSPICE}>:${NGSPICE}>)
  endforeach()
  # spice::flatten(), the reader's view for LVS and characterization.
  set(flat ${CMAKE_SOURCE_DIR}/tests/spice/flat)
  add_test(NAME spice/flatten
           COMMAND ${Python3_EXECUTABLE} ${flat}/check.py $<TARGET_FILE:mspice> ${flat}/flat.cir ${flat}/flat.expected -flatten)
  add_test(NAME spice/flatten_top
           COMMAND ${Python3_EXECUTABLE} ${flat}/check.py $<TARGET_FILE:mspice> ${flat}/flat.cir ${flat}/flat_top.expected
                   -flatten -top TOP)
endif()
