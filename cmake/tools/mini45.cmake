include_guard(GLOBAL)
# mini45, the generic cell library (lib/mini45, docs/MINI45.md), is
# generated: the files in the tree must be what the generator writes.
# Its Verilog models are checked with msim (mini45_models, in the slang build).
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
  add_test(NAME mini45_gen
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/lib/mini45/gen_mini45.py --check)
endif()

include(GNUInstallDirs)
install(DIRECTORY lib/mini45 DESTINATION ${CMAKE_INSTALL_DATADIR}/minisim/lib
        FILES_MATCHING PATTERN "*.lib" PATTERN "*.lef" PATTERN "mini45.v")
install(FILES docs/MINI45.md DESTINATION ${CMAKE_INSTALL_DOCDIR})
