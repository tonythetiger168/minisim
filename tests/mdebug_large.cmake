# mdebug on a design with many signals (bench/gen_soc.py: 256 cores, about
# 6000 signals). mdebug reads a waveform on demand: a query reads only the
# signals it looks at, and the one-file page reads the waveform a block of
# 2048 signals at a time. The page must be the same from the VCD and from
# the FST of one run, and for any thread count; a query gives the value the
# page holds.
#   cmake -DMSIM=<msim> -DMDEBUG=<mdebug> -DPY=<python> -DSRC=<source dir> -DOUT=<dir> [-DFST=1] -P mdebug_large.cmake
file(MAKE_DIRECTORY ${OUT})
execute_process(COMMAND ${PY} ${SRC}/bench/gen_soc.py 256 300 OUTPUT_FILE ${OUT}/soc.sv RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "gen_soc.py failed")
endif()
set(SV ${OUT}/soc.sv)
execute_process(COMMAND ${MSIM} ${SV} -top soc_tb -vcd ${OUT}/soc.vcd RESULT_VARIABLE rc OUTPUT_QUIET ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "msim -vcd exited ${rc}: ${err}")
endif()

function(page out)
  execute_process(COMMAND ${MDEBUG} ${SV} -top soc_tb ${ARGN} -json ${OUT}/${out}
                  RESULT_VARIABLE rc OUTPUT_QUIET ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "mdebug ${ARGN} exited ${rc}: ${err}")
  endif()
  file(READ ${OUT}/${out} j)
  string(REGEX REPLACE "\"files\":.*\"signals\":" "" j "${j}")   # source paths differ in nothing, but keep it short
  set(page "${j}" PARENT_SCOPE)
endfunction()

page(v1.json -vcd ${OUT}/soc.vcd -threads 1)
set(v1 "${page}")
string(LENGTH "${v1}" n)
if(n LESS 100000 OR NOT v1 MATCHES "\"waves\":\\{\"0\":")
  message(FATAL_ERROR "mdebug -json: no waves in the page (${n} bytes)")
endif()
page(v4.json -vcd ${OUT}/soc.vcd -threads 4)
if(NOT v1 STREQUAL page)
  message(FATAL_ERROR "mdebug -json: the page depends on the thread count")
endif()

# A query: the value at a time, as the page has it.
file(WRITE ${OUT}/q.txt "value soc_tb.dut.k7.c3.acc 1000\nchanges soc_tb.dut.k7.c3.st 0 60\nend\n")
execute_process(COMMAND ${MDEBUG} ${SV} -top soc_tb -vcd ${OUT}/soc.vcd -script ${OUT}/q.txt OUTPUT_VARIABLE q RESULT_VARIABLE rc)
foreach(want "\"signal\":\"soc_tb.dut.k7.c3.acc\",\"time\":1000,\"value\":\"32'h[0-9a-f]+\""
             "\"changes\":\\[\\[0,\"00\"\\],\\[15,\"01\"\\],\\[25,\"10\"\\],\\[35,\"11\"\\],\\[45,\"00\"\\],\\[55,\"01\"\\]\\]"
             "\"end\":3[0-9][0-9][0-9]")
  if(NOT q MATCHES "${want}")
    message(FATAL_ERROR "mdebug -script: no match for '${want}' in\n${q}")
  endif()
endforeach()

if(FST)
  execute_process(COMMAND ${MSIM} ${SV} -top soc_tb -fst ${OUT}/soc.fst RESULT_VARIABLE rc OUTPUT_QUIET ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "msim -fst exited ${rc}: ${err}")
  endif()
  page(f3.json -fst ${OUT}/soc.fst -threads 3)
  if(NOT v1 STREQUAL page)
    message(FATAL_ERROR "mdebug -json: the page from the FST is not the page from the VCD")
  endif()
  execute_process(COMMAND ${MDEBUG} ${SV} -top soc_tb -fst ${OUT}/soc.fst -script ${OUT}/q.txt OUTPUT_VARIABLE qf)
  if(NOT q STREQUAL qf)
    message(FATAL_ERROR "mdebug -script: the FST answers differ\n${q}\n${qf}")
  endif()
endif()
