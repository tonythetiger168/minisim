# mlibc self-test: mini45 is clean at every corner and against its LEF, every
# rule fires on tests/libc/bad.lib, and the output does not depend on -threads.
#   cmake -DMLIBC=<mlibc> -DSRC=<source dir> -P mlibc.cmake
set(M45 ${SRC}/lib/mini45)
function(run want_rc)
  execute_process(COMMAND ${MLIBC} ${ARGN} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  set(out "${out}" PARENT_SCOPE)
  if(NOT rc EQUAL ${want_rc})
    message(FATAL_ERROR "mlibc ${ARGN} exited ${rc}, expected ${want_rc}\n${out}${err}")
  endif()
endfunction()

run(0 check ${M45}/mini45_tt.lib ${M45}/mini45_ss.lib ${M45}/mini45_ff.lib
      -lef ${M45}/mini45_tech.lef -lef ${M45}/mini45.lef -Werror)
foreach(c tt ss ff)
  if(NOT out MATCHES "mini45_${c}.lib: 88 cells, 0 findings")
    message(FATAL_ERROR "mini45_${c}.lib is not clean:\n${out}")
  endif()
endforeach()

run(1 check ${SRC}/tests/libc/bad.lib -lef ${SRC}/tests/libc/bad.lef -Werror)
set(all "${out}")
foreach(want
    "NOFUNC: [LIB001]" "SENSE: [LIB002]" "SENSE: [LIB003]" "SENSE: [LIB004]" "TABLES: [LIB005]"
    "TABLES: [LIB006]" "NOFUNC: [LIB007]" "pin D has no hold arc" "pin CK clocks the cell"
    "NOFUNC: [LIB009]" "LEAK: [LIB010]" "FLOP: [LIB011]" "LEAK: [LIB012]" "NOFUNC: [LIB012]"
    "GOOD_X2: [LIB013]" "TABLES: [LIB014]" "LEAK: [LIB015]"
    "NOFUNC: [LEF001]" "GOOD_X2: [LEF002]" "pin A is OUTPUT in the macro" "pin Z is not in the Liberty cell"
    "pin C is not in the macro" "SENSE: [LEF004]" "SENSE: [LEF005]"
    "bad.lib: 7 cells, 31 findings")
  string(FIND "${out}" "${want}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing: ${want}\n${out}")
  endif()
endforeach()
if(out MATCHES "GOOD_X1: ")
  message(FATAL_ERROR "the clean cell was reported:\n${out}")
endif()
# The same text for any thread count.
foreach(n 1 3)
  run(1 check ${SRC}/tests/libc/bad.lib -lef ${SRC}/tests/libc/bad.lef -Werror -threads ${n})
  if(NOT out STREQUAL all)
    message(FATAL_ERROR "-threads ${n} changed the output:\n${out}")
  endif()
endforeach()
# -disable drops a rule.
run(1 check ${SRC}/tests/libc/bad.lib -Werror -disable LIB003)
if(out MATCHES "LIB003" OR NOT out MATCHES "LIB002")
  message(FATAL_ERROR "-disable LIB003:\n${out}")
endif()

# Corners: slow against fast is clean, the other way round is not.
run(0 compare ${M45}/mini45_ss.lib ${M45}/mini45_ff.lib -Werror)
if(NOT out MATCHES "88 cells, 210 arcs compared, slow/fast delay ratio [12]\\.[0-9]+, 0 findings")
  message(FATAL_ERROR "compare ss ff:\n${out}")
endif()
run(1 compare ${M45}/mini45_ff.lib ${M45}/mini45_ss.lib -Werror)
if(NOT out MATCHES "INV_X1: .CMP002.")
  message(FATAL_ERROR "compare ff ss:\n${out}")
endif()
run(1 compare ${M45}/mini45_tt.lib ${SRC}/tests/libc/bad.lib -Werror)
if(NOT out MATCHES "INV_X1: .CMP001." OR NOT out MATCHES "NOFUNC: .CMP001.")
  message(FATAL_ERROR "compare with another library:\n${out}")
endif()

run(0 report ${M45}/mini45_tt.lib)
if(NOT out MATCHES "library mini45_tt: 88 cells, 1.1 V, 25 C" OR NOT out MATCHES "NAND2_X1 +0.798 +2.4 +3 +0.00[0-9]+"
   OR NOT out MATCHES "total area 193.648")
  message(FATAL_ERROR "report:\n${out}")
endif()
run(0 report ${M45}/mini45_tt.lib -cell DFFR_X1)
if(NOT out MATCHES "pin CK input cap [0-9.]+ pF clock" OR NOT out MATCHES "arc CK -> D setup_rising"
   OR NOT out MATCHES "arc RN -> Q clear")
  message(FATAL_ERROR "report -cell:\n${out}")
endif()
run(2 check ${SRC}/tests/libc/no_such.lib)
