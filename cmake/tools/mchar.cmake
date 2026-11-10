# mchar: cell library characterization on mspice (docs/CHAR.md). Reads and
# writes Liberty with src/liberty.hpp, simulates with src/spice.hpp.
find_package(Threads REQUIRED)
add_executable(mchar src/mchar.cpp)
target_include_directories(mchar PRIVATE src)
target_link_libraries(mchar PRIVATE Threads::Threads)
install(TARGETS mchar RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES docs/CHAR.md DESTINATION ${CMAKE_INSTALL_DOCDIR})

# A cell with a hand-worked answer, and a few mini45 cells measured again:
# the same for any thread count, and the numbers the tree has.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
  add_test(NAME mchar
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/mchar_check.py $<TARGET_FILE:mchar>
                   ${CMAKE_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/mchar_test)
endif()
