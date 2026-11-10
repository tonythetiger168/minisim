# mlibc: cell library checks and reports on the shared Liberty and LEF
# readers (docs/LIBCHECK.md). Plain C++20 and threads: no slang.
find_package(Threads REQUIRED)
add_executable(mlibc src/mlibc.cpp)
target_include_directories(mlibc PRIVATE src)
target_link_libraries(mlibc PRIVATE Threads::Threads)

# mini45 clean at three corners and against its LEF, every rule on
# tests/libc/bad.lib, the same output for any -threads.
add_test(NAME mlibc
         COMMAND ${CMAKE_COMMAND} -DMLIBC=$<TARGET_FILE:mlibc> -DSRC=${CMAKE_SOURCE_DIR}
                 -P ${CMAKE_SOURCE_DIR}/tests/mlibc.cmake)

include(GNUInstallDirs)
install(TARGETS mlibc RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES docs/LIBCHECK.md DESTINATION ${CMAKE_INSTALL_DOCDIR})
