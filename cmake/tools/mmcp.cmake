# mmcp: the minisim engines as MCP tools for AI agents (docs/MCP.md).
# Python standard library only. The test runs the protocol and plug-in checks
# everywhere, and the engine checks when msim, mlint and mdebug are built.
include_guard(GLOBAL)
include(GNUInstallDirs)
install(PROGRAMS tools/mmcp.py DESTINATION ${CMAKE_INSTALL_BINDIR} RENAME mmcp)
install(DIRECTORY DESTINATION ${CMAKE_INSTALL_DATADIR}/minisim/mcp)
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
  if(TARGET msim AND TARGET mlint AND TARGET mdebug)
    add_test(NAME mmcp
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/mmcp_test.py ${CMAKE_SOURCE_DIR} $<TARGET_FILE_DIR:msim>)
  else()
    add_test(NAME mmcp
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/mmcp_test.py ${CMAKE_SOURCE_DIR})
  endif()
endif()
