# Physical verification: mpv drc (GDSII in, rule deck, violation markers out).
# test_pv checks the polygon engine, the GDSII reader and writer and every
# check; the mpv test runs the command on SKY130 standard cells (clean) and
# on a layout with known violations.
find_package(Threads REQUIRED)
add_executable(mpv src/mpv.cpp)
target_include_directories(mpv PRIVATE src)
target_link_libraries(mpv PRIVATE Threads::Threads)
add_executable(test_pv tests/test_pv.cpp)
target_include_directories(test_pv PRIVATE src)
target_compile_options(test_pv PRIVATE -UNDEBUG)
target_link_libraries(test_pv PRIVATE Threads::Threads)
add_test(NAME test_pv COMMAND test_pv)
add_test(NAME mpv
         COMMAND ${CMAKE_COMMAND} -DMPV=$<TARGET_FILE:mpv> -DGEN=$<TARGET_FILE:test_pv>
                 -DSRC=${CMAKE_SOURCE_DIR} -DOUT=${CMAKE_CURRENT_BINARY_DIR}/mpv_test
                 -P ${CMAKE_SOURCE_DIR}/tests/mpv.cmake)

include(GNUInstallDirs)
install(TARGETS mpv RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES docs/MPV.md DESTINATION ${CMAKE_INSTALL_DOCDIR})
