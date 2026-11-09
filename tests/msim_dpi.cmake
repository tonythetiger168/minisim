# DPI-C command line (IEEE 1800-2023 annex J): a prebuilt library through
# -sv_lib / -sv_root and through -sv_liblist, -dpiheader, -o keeping the
# build, and an import with no C function failing with a clear message.
set(dir ${CMAKE_CURRENT_BINARY_DIR}/msim_dpi_test)
file(REMOVE_RECURSE ${dir})
file(MAKE_DIRECTORY ${dir}/lib)
set(spec ${SRC}/tests/spec)
find_program(CC NAMES cc gcc clang REQUIRED)
execute_process(COMMAND ${CC} -shared -fPIC -I${SRC}/include ${spec}/data/dpi/decl.c -o ${dir}/lib/decl.so
                RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "cc failed")
endif()

function(expect_run what want)
  execute_process(COMMAND ${MSIM} ${ARGN} WORKING_DIRECTORY ${dir} RESULT_VARIABLE rc
                  OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT rc EQUAL 0 OR NOT out MATCHES "${want}")
    message(FATAL_ERROR "${what}: exit ${rc}\n${out}\n${err}")
  endif()
endfunction()

expect_run("-sv_lib" "hello from C\n6\n" ${spec}/35.5.4-import-decl.sv -sv_root lib -sv_lib decl)
expect_run("-sv_lib with .so" "hello from C\n6\n" ${spec}/35.5.4-import-decl.sv -sv_lib lib/decl.so)
file(WRITE ${dir}/boot "#!SV_LIBRARIES\n# a comment\nlib/decl\n")
expect_run("-sv_liblist" "hello from C\n6\n" ${spec}/35.5.4-import-decl.sv -sv_liblist boot)

# -dpiheader and -o
expect_run("-dpiheader" "C: step 2" ${spec}/35.8-export-tasks.sv ${spec}/data/dpi/tasks.c
           -dpiheader hdr.h -o build)
file(READ ${dir}/hdr.h h)
foreach(want "extern int run_seq(int n);" "extern int sv_drive(int v, int* echo);" "extern int sv_now(void);")
  string(FIND "${h}" "${want}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "hdr.h lacks '${want}':\n${h}")
  endif()
endforeach()
if(NOT EXISTS ${dir}/build/dpi_user.so OR NOT EXISTS ${dir}/build/dpi_exports.so)
  message(FATAL_ERROR "-o build did not keep the DPI libraries")
endif()

# the header compiles, and agrees with the C file
execute_process(COMMAND ${CC} -fsyntax-only -include ${dir}/hdr.h -I${SRC}/include ${spec}/data/dpi/tasks.c
                RESULT_VARIABLE rc ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "hdr.h does not match tasks.c:\n${err}")
endif()

# no C function: an error naming the import
file(WRITE ${dir}/missing.sv "module t;\n  import \"DPI-C\" function int no_such_fn(int a);\n  initial $display(\"%0d\", no_such_fn(1));\nendmodule\n")
execute_process(COMMAND ${MSIM} missing.sv WORKING_DIRECTORY ${dir} RESULT_VARIABLE rc ERROR_VARIABLE err OUTPUT_QUIET)
if(rc EQUAL 0 OR NOT err MATCHES "no C function \"no_such_fn\"")
  message(FATAL_ERROR "missing import: exit ${rc}: ${err}")
endif()
