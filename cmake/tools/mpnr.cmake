# mpnr: place and route, gate netlist + LEF in, placed and routed DEF out
# (docs/PNR.md). Plain C++20 and threads: no slang, so it builds in every
# configuration.
include_guard(GLOBAL)
find_package(Threads REQUIRED)
add_executable(mpnr src/mpnr.cpp)
target_include_directories(mpnr PRIVATE src)
target_link_libraries(mpnr PRIVATE Threads::Threads)

# The command line on tests/pnr: placement, routing, floorplan DEF, the
# layout check, thread counts.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
  add_test(NAME mpnr
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/pnr_test.py $<TARGET_FILE:mpnr> ${CMAKE_SOURCE_DIR}
                   ${CMAKE_CURRENT_BINARY_DIR}/pnr_test)
endif()

include(GNUInstallDirs)
install(TARGETS mpnr RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
