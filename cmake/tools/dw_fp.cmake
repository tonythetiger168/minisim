# Floating point datapath modules (dw/MW_fp_*.sv): tests/dw_fp_test.py runs
# them on msim in five formats and five rounding modes against an exact
# rational reference.
include_guard(GLOBAL)
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND AND TARGET msim)
  add_test(NAME dw/fp
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/dw_fp_test.py
                   ${CMAKE_SOURCE_DIR} $<TARGET_FILE:msim>)
  set_tests_properties(dw/fp PROPERTIES LABELS "dw" TIMEOUT 600)
endif()
