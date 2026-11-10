# mrcx: parasitic RC extraction, routed DEF + LEF in, SPEF out (docs/RCX.md).
# Plain C++20 and threads: no slang, so it builds in every configuration.
find_package(Threads REQUIRED)
add_executable(mrcx src/mrcx.cpp)
target_include_directories(mrcx PRIVATE src)
target_link_libraries(mrcx PRIVATE Threads::Threads)

# The engine (src/rcx.hpp) and the SPEF reader and writer (src/spef.hpp)
# against values worked out by hand.
add_executable(test_rcx tests/test_rcx.cpp)
target_include_directories(test_rcx PRIVATE src)
target_compile_options(test_rcx PRIVATE -UNDEBUG)
target_link_libraries(test_rcx PRIVATE Threads::Threads)
add_test(NAME test_rcx COMMAND test_rcx)

# The command line on tests/rcx: SPEF contents, options, thread counts.
add_test(NAME mrcx
         COMMAND ${CMAKE_COMMAND} -DMRCX=$<TARGET_FILE:mrcx> -DSRC=${CMAKE_SOURCE_DIR}
                 -DOUT=${CMAKE_CURRENT_BINARY_DIR}/mrcx_test -P ${CMAKE_SOURCE_DIR}/tests/mrcx.cmake)

include(GNUInstallDirs)
install(TARGETS mrcx RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
