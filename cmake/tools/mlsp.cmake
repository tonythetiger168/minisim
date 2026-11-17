# mlsp: the language server (docs/LSP.md) and the VS Code extension that
# starts it (editors/vscode/). mlsp needs slang, the front end msim uses.
include_guard(GLOBAL)
if(MINISIM_WITH_SLANG)
  find_package(Threads REQUIRED)
  add_executable(mlsp src/mlsp.cpp)
  target_include_directories(mlsp PRIVATE src ${CMAKE_BINARY_DIR}/gen)
  target_link_libraries(mlsp PRIVATE slang::slang Threads::Threads)

  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_FOUND)
    # The protocol end to end: a client written in Python drives mlsp
    # (diagnostics, definition, references, hover, outline, symbols,
    # completion, unsaved buffers, -threads 1 vs N); mlint on save when built.
    add_test(NAME mlsp
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/mlsp_test.py $<TARGET_FILE:mlsp>
                     ${CMAKE_SOURCE_DIR}/tests/mlsp $<$<TARGET_EXISTS:mlint>:$<TARGET_FILE:mlint>>)
    set_tests_properties(mlsp PROPERTIES TIMEOUT 300)
  endif()

  include(GNUInstallDirs)
  install(TARGETS mlsp RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  install(FILES docs/LSP.md DESTINATION ${CMAKE_INSTALL_DOCDIR})
endif()

# The extension's own checks (TypeScript compile and its unit tests) run only
# where node and its packages are installed: tests/vscode_ext.sh skips (77)
# otherwise.
find_program(MLSP_BASH bash)
if(MLSP_BASH)
  add_test(NAME vscode_ext COMMAND ${MLSP_BASH} ${CMAKE_SOURCE_DIR}/tests/vscode_ext.sh ${CMAKE_SOURCE_DIR}/editors/vscode)
  set_tests_properties(vscode_ext PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)
endif()
