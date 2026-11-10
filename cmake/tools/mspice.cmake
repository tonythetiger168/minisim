# mspice: the SPICE circuit simulator. No dependencies beyond the C++ library.
add_executable(mspice src/mspice.cpp)
target_include_directories(mspice PRIVATE src)
find_package(Threads REQUIRED)
target_link_libraries(mspice PRIVATE Threads::Threads)
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
