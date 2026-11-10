# Coverage: one run of tests/cov/cov.sv misses the default arm and the else
# branch; merged with a second run, line and branch coverage reach 100%.
# Also checks mcov's thresholds, LCOV and HTML output, and mregress -cov.
#   cmake -DPY=<python3> -DMSIM=<msim> -DSRC=<source dir> -DOUT=<dir> -P msim_cov.cmake
file(REMOVE_RECURSE ${OUT})
file(MAKE_DIRECTORY ${OUT})
set(SV ${SRC}/tests/cov/cov.sv)
set(MCOV ${PY} ${SRC}/tools/mcov.py)
foreach(sel 1 3)
  execute_process(COMMAND ${MSIM} ${SV} +define+SEL=${sel} -cov s${sel}.json WORKING_DIRECTORY ${OUT}
                  RESULT_VARIABLE rc OUTPUT_QUIET ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "msim -cov exited ${rc}: ${err}")
  endif()
endforeach()
execute_process(COMMAND ${MCOV} report s1.json -min-line 100 WORKING_DIRECTORY ${OUT}
                RESULT_VARIABLE rc OUTPUT_VARIABLE out)
if(NOT rc EQUAL 1 OR NOT out MATCHES "Total +88\\.9% +8/9 +66\\.7% +4/6")
  message(FATAL_ERROR "single run: expected 8/9 lines, 4/6 branches and exit 1 (rc ${rc})\n${out}")
endif()
execute_process(COMMAND ${MCOV} merge s1.json s3.json -o all.json WORKING_DIRECTORY ${OUT} RESULT_VARIABLE rc OUTPUT_QUIET)
execute_process(COMMAND ${MCOV} report all.json -min-line 100 -min-branch 100 -lcov all.info -html html
                WORKING_DIRECTORY ${OUT} RESULT_VARIABLE rc OUTPUT_VARIABLE out)
if(NOT rc EQUAL 0 OR NOT out MATCHES "Total +100\\.0% +9/9 +100\\.0% +6/6")
  message(FATAL_ERROR "merged: expected 9/9 lines and 6/6 branches (rc ${rc})\n${out}")
endif()
file(READ ${OUT}/all.info info)
if(NOT info MATCHES "DA:9,1" OR NOT info MATCHES "LH:9")
  message(FATAL_ERROR "LCOV output wrong:\n${info}")
endif()
if(NOT EXISTS ${OUT}/html/index.html)
  message(FATAL_ERROR "no HTML report")
endif()
execute_process(COMMAND ${PY} ${SRC}/tools/mregress.py ${SRC}/tests/cov/cov.list -exe ${MSIM} -cov -o ${OUT}/rg -q
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT out MATCHES "Total +100\\.0% +9/9" OR NOT EXISTS ${OUT}/rg/cov.json)
  message(FATAL_ERROR "mregress -cov (rc ${rc})\n${out}${err}")
endif()

# msim -compile -cov: the generated code counts the same hits, so the
# database reports the same as the interpreter's (s1.json above). A second
# run reuses the build (the build cache in -o) and gets another seed.
foreach(run 1 2)
  execute_process(COMMAND ${MSIM} -compile -o cdir ${SV} +define+SEL=1 -cov c${run}.json -seed ${run}
                  WORKING_DIRECTORY ${OUT} RESULT_VARIABLE rc OUTPUT_QUIET ERROR_VARIABLE err)
  if(NOT rc EQUAL 0 OR NOT EXISTS ${OUT}/c${run}.json)
    message(FATAL_ERROR "msim -compile -cov exited ${rc}: ${err}")
  endif()
endforeach()
execute_process(COMMAND ${MCOV} report s1.json WORKING_DIRECTORY ${OUT} OUTPUT_VARIABLE interp)
execute_process(COMMAND ${MCOV} report c2.json WORKING_DIRECTORY ${OUT} OUTPUT_VARIABLE comp)
string(REGEX REPLACE "[^\n]*run\\(s\\)\n" "" interp "${interp}")
string(REGEX REPLACE "[^\n]*run\\(s\\)\n" "" comp "${comp}")
if(NOT interp STREQUAL comp)
  message(FATAL_ERROR "-compile coverage differs from the interpreter's\n--- interpreter\n${interp}--- compiled\n${comp}")
endif()

# mcov loads many databases in parallel worker processes; the result is the
# same as one at a time.
execute_process(COMMAND ${MCOV} merge s1.json s3.json s1.json s3.json s1.json s3.json s1.json s3.json -j 4 -o par.json
                WORKING_DIRECTORY ${OUT} RESULT_VARIABLE rc OUTPUT_QUIET)
execute_process(COMMAND ${MCOV} merge s1.json s3.json s1.json s3.json s1.json s3.json s1.json s3.json -j 1 -o ser.json
                WORKING_DIRECTORY ${OUT} RESULT_VARIABLE rc2 OUTPUT_QUIET)
file(READ ${OUT}/par.json par)
file(READ ${OUT}/ser.json ser)
if(NOT rc EQUAL 0 OR NOT rc2 EQUAL 0 OR NOT par STREQUAL ser)
  message(FATAL_ERROR "mcov merge -j 4 differs from -j 1")
endif()

# Functional coverage in the database: covergroup bins and assertion
# statistics (tests/cov/counter.sv). One run leaves holes; merged with the
# other modes every bin is covered and every assertion passed or was hit.
set(CSV ${SRC}/tests/cov/counter.sv)
foreach(mode 0 1 2)
  execute_process(COMMAND ${MSIM} ${CSV} -top tb +define+MODE=${mode} -cov f${mode}.json WORKING_DIRECTORY ${OUT}
                  RESULT_VARIABLE rc OUTPUT_QUIET ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "msim -cov (counter, mode ${mode}) exited ${rc}: ${err}")
  endif()
endforeach()
file(READ ${OUT}/f1.json db)
if(NOT db MATCHES "\\[\"cp_q\", \"point\", 1, 100, 1, 2, \\[\\[\"lo\", 10\\], \\[\"hi\", 4\\]\\]\\]"
   OR NOT db MATCHES "\\[\"x_q_dir\", \"cross\", 1, 100, 1, 4, \\[\\[\"<lo,up>\", 6\\], \\[\"<hi,up>\", 4\\]\\]\\]"
   OR NOT db MATCHES "\\[\"tb.dut.c_wrap\", \"cover\", 15, 1, 14\\]")
  message(FATAL_ERROR "covergroups / assertions missing from the database:\n${db}")
endif()
execute_process(COMMAND ${MCOV} report f0.json -min-func 80 -min-assert 100 -html fhtml WORKING_DIRECTORY ${OUT}
                RESULT_VARIABLE rc OUTPUT_VARIABLE out)
if(NOT rc EQUAL 1 OR NOT out MATCHES "tb\\.cg +41\\.7% +3/8 +bins" OR NOT out MATCHES "Functional: 41\\.7%"
   OR NOT out MATCHES "Assertions: 66\\.7% \\(2/3 passed or hit, 1 never passed or hit\\)"
   OR NOT out MATCHES "func coverage 41\\.7% is below 80" OR NOT out MATCHES "assert coverage 66\\.7% is below 100")
  message(FATAL_ERROR "functional coverage of one run wrong (rc ${rc})\n${out}")
endif()
file(READ ${OUT}/fhtml/index.html page)
if(NOT page MATCHES "Functional coverage: 41\\.7%" OR NOT page MATCHES "tb\\.dut\\.c_wrap</td><td>cover</td><td>never hit")
  message(FATAL_ERROR "functional coverage missing from the HTML report")
endif()
execute_process(COMMAND ${MCOV} merge f0.json f1.json f2.json -o fall.json WORKING_DIRECTORY ${OUT} RESULT_VARIABLE rc OUTPUT_QUIET)
execute_process(COMMAND ${MCOV} report fall.json -min-func 100 -min-assert 100 WORKING_DIRECTORY ${OUT}
                RESULT_VARIABLE rc OUTPUT_VARIABLE out)
if(NOT rc EQUAL 0 OR NOT out MATCHES "Functional: 100\\.0%" OR NOT out MATCHES "Assertions: 100\\.0% \\(3/3")
  message(FATAL_ERROR "merged functional coverage wrong (rc ${rc})\n${out}")
endif()
file(READ ${OUT}/fall.json db)
if(NOT db MATCHES "\\[\"tb.dut.a_rst\", \"assert\", [0-9]+, [0-9]+, 0\\]" OR NOT db MATCHES "\"<lo,down>\", [1-9]")
  message(FATAL_ERROR "merged database lost the functional sections:\n${db}")
endif()
