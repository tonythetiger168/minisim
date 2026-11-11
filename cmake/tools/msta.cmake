# Static timing analysis of gate netlists: msta -lib cells.lib netlist.v -sdc top.sdc
# (docs/STA.md). Plain C++20 and threads: no slang, so it builds in every
# configuration. The guard makes a second include harmless.
include_guard(GLOBAL)
find_package(Threads REQUIRED)
find_package(Python3 COMPONENTS Interpreter QUIET)
add_executable(msta src/msta.cpp)
target_include_directories(msta PRIVATE src)
target_link_libraries(msta PRIVATE Threads::Threads)
add_test(NAME msta
         COMMAND ${CMAKE_COMMAND} -DMSTA=$<TARGET_FILE:msta> -DSRC=${CMAKE_SOURCE_DIR}
                 -DOUT=${CMAKE_CURRENT_BINARY_DIR}/msta_test -DPYTHON=${Python3_EXECUTABLE}
                 -P ${CMAKE_SOURCE_DIR}/tests/msta.cmake)

include(GNUInstallDirs)
install(TARGETS msta RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
