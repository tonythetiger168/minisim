# Multicore msim against an independent oracle, and a scheduling-sensitive
# stability check the oracle cannot settle.
#
# Icarus Verilog (iverilog + vvp) is the oracle, the same one the icarus/*
# spec tests use. tests/msim_threads.cmake compares bench/wide.sv only with
# msim's own single-thread event-driven run. The designs here are ones where
# a wrong parallel commit changes the result:
#   lanes, wide   many independent partitions and clocked processes
#                 (wide is bench/wide.sv at END_TIME=5000, the same horizon
#                 tests/msim_threads.cmake uses, 500 cycles of 256 lanes)
#   pipe          NBA pipeline; an early write skips a stage
#   regions       active / #0 / $strobe around a wide parallel NBA
#   combs         same-level combinational ranks, committed before the next level
#   watch         one lane forced serial by a waiter, the rest parallel
# Each of those has 64 (wide: 256) parallel items. The kernel runs a batch on
# the calling thread when it is smaller than 32, so a smaller design would
# not exercise the pool. Requested thread counts above the machine's hardware
# threads are capped by msim; stdout is still compared.
#
# lastwin writes one variable from 64 processes in one slot, and also sets a
# distinct bit per process. The bits and the pre-NBA sample are checked
# against Icarus. The LRM leaves the overlapping write's process order
# unspecified, so a different winner from Icarus is not a failure. A winner
# that changes with the thread count, or a torn (shared, mix) pair, is.
#
# Called with -DMSIM=<msim> -DSRC=<source dir> -DIVERILOG=<iverilog> -DVVP=<vvp> -DOUT=<scratch>.
if(NOT MSIM OR NOT SRC OR NOT IVERILOG OR NOT VVP OR NOT OUT)
  message(FATAL_ERROR "msim_threads_ref needs MSIM, SRC, IVERILOG, VVP, and OUT")
endif()
file(MAKE_DIRECTORY ${OUT})

execute_process(COMMAND nproc OUTPUT_VARIABLE ncpu OUTPUT_STRIP_TRAILING_WHITESPACE)

function(normalize_sim_output raw outvar)
  set(s "${raw}")
  string(REGEX REPLACE "[^\n]*\\$finish called[^\n]*\n" "" s "${s}")
  string(REGEX REPLACE "[^\n]*\\$finish at[^\n]*\n" "" s "${s}")
  string(REGEX REPLACE "VCD info:[^\n]*\n" "" s "${s}")
  string(REGEX REPLACE "WARNING: [^\n]*\n" "" s "${s}")
  set(${outvar} "${s}" PARENT_SCOPE)
endfunction()

# run_msim(<outvar> <sv> [extra msim args...])
function(run_msim outvar sv)
  execute_process(COMMAND ${MSIM} ${ARGN} ${sv}
                  OUTPUT_VARIABLE got RESULT_VARIABLE rc TIMEOUT 60)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "msim ${ARGN} ${sv} exited ${rc}:\n${got}")
  endif()
  normalize_sim_output("${got}" norm)
  set(${outvar} "${norm}" PARENT_SCOPE)
endfunction()

# run_icarus(<outvar> <sv> [iverilog args before the file, e.g. -DEND_TIME=5000])
function(run_icarus outvar sv)
  get_filename_component(name ${sv} NAME_WE)
  set(vvp ${OUT}/icarus_${name}.vvp)
  execute_process(COMMAND ${IVERILOG} -g2012 ${ARGN} -o ${vvp} ${sv}
                  RESULT_VARIABLE rc ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "iverilog failed on ${sv}:\n${err}")
  endif()
  execute_process(COMMAND ${VVP} -n ${vvp}
                  OUTPUT_VARIABLE got RESULT_VARIABLE rc TIMEOUT 60)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "vvp failed on ${sv} (${rc}):\n${got}")
  endif()
  normalize_sim_output("${got}" norm)
  set(${outvar} "${norm}" PARENT_SCOPE)
endfunction()

# expect_match(<label> <oracle> <sv> [msim args...])
function(expect_match label oracle sv)
  run_msim(got ${sv} ${ARGN})
  if(NOT got STREQUAL oracle)
    message(FATAL_ERROR "${label} differs from Icarus on ${sv}\n--- icarus\n${oracle}--- msim\n${got}")
  endif()
endfunction()

# check_oracle(<name> <sv> [one iverilog -D, then --, then msim extra args])
# No extras: check_oracle(lanes ${SRC}/tests/threads/lanes.sv)
function(check_oracle name sv)
  set(iv_args "")
  set(msim_args "")
  set(which iv)
  foreach(a IN LISTS ARGN)
    if(a STREQUAL "--")
      set(which msim)
    elseif(which STREQUAL "iv")
      list(APPEND iv_args ${a})
    else()
      list(APPEND msim_args ${a})
    endif()
  endforeach()
  run_icarus(oracle ${sv} ${iv_args})
  if(oracle STREQUAL "")
    message(FATAL_ERROR "Icarus produced no output for ${sv}")
  endif()
  expect_match("${name}/threads-1" "${oracle}" ${sv} -threads 1 ${msim_args})
  expect_match("${name}/threads-2" "${oracle}" ${sv} -threads 2 ${msim_args})
  expect_match("${name}/threads-3" "${oracle}" ${sv} -threads 3 ${msim_args})
  expect_match("${name}/threads-4" "${oracle}" ${sv} -threads 4 ${msim_args})
  expect_match("${name}/threads-default" "${oracle}" ${sv} ${msim_args})
  expect_match("${name}/event-driven" "${oracle}" ${sv} -threads 1 -no-static-comb -no-static-seq -no-partition ${msim_args})
  # A second pass, to catch a commit that only sometimes lands out of order.
  expect_match("${name}/threads-4-repeat" "${oracle}" ${sv} -threads 4 ${msim_args})
  expect_match("${name}/threads-default-repeat" "${oracle}" ${sv} ${msim_args})
  message(STATUS "${name} matches Icarus at threads 1-4, the default, and event-driven")
endfunction()

check_oracle(lanes ${SRC}/tests/threads/lanes.sv)
check_oracle(pipe ${SRC}/tests/threads/pipe.sv)
check_oracle(regions ${SRC}/tests/threads/regions.sv)
check_oracle(combs ${SRC}/tests/threads/combs.sv)
check_oracle(watch ${SRC}/tests/threads/watch.sv)
check_oracle(wide ${SRC}/bench/wide.sv -DEND_TIME=5000 -- +define+END_TIME=5000)

# lastwin: stable across modes. The pre-NBA line and the order-independent
# `hit` vector are checked against Icarus. Which process's write survives in
# shared/mix is reported; a different winner from Icarus is not a failure.
set(sv ${SRC}/tests/threads/lastwin.sv)
run_icarus(icarus_last ${sv})
run_msim(last_ref ${sv} -threads 1)
function(split_lines text out0 out1 out2)
  string(REGEX MATCHALL "[^\n]+" lines "${text}")
  list(LENGTH lines n)
  if(NOT n EQUAL 3)
    message(FATAL_ERROR "lastwin: expected 3 lines, got ${n}:\n${text}")
  endif()
  list(GET lines 0 l0)
  list(GET lines 1 l1)
  list(GET lines 2 l2)
  set(${out0} "${l0}" PARENT_SCOPE)
  set(${out1} "${l1}" PARENT_SCOPE)
  set(${out2} "${l2}" PARENT_SCOPE)
endfunction()
function(hit_field line outvar)
  if(NOT line MATCHES "^[^ ]+ [0-9]+ [0-9]+ ([0-9a-fA-F]+)$")
    message(FATAL_ERROR "lastwin: unparsed line: ${line}")
  endif()
  set(${outvar} "${CMAKE_MATCH_1}" PARENT_SCOPE)
endfunction()
split_lines("${icarus_last}" ic0 ic1 ic2)
split_lines("${last_ref}" ms0 ms1 ms2)
if(NOT ic0 STREQUAL ms0)
  message(FATAL_ERROR "lastwin pre-NBA line differs from Icarus\n--- icarus\n${ic0}\n--- msim\n${ms0}")
endif()
hit_field("${ic1}" ic_hit1)
hit_field("${ic2}" ic_hit2)
hit_field("${ms1}" ms_hit1)
hit_field("${ms2}" ms_hit2)
if(NOT ic_hit1 STREQUAL "ffffffffffffffff" OR NOT ic_hit2 STREQUAL "ffffffffffffffff")
  message(FATAL_ERROR "lastwin: Icarus did not record all 64 bit writes:\n${icarus_last}")
endif()
if(NOT ms_hit1 STREQUAL ic_hit1 OR NOT ms_hit2 STREQUAL ic_hit2)
  message(FATAL_ERROR "lastwin hit vector differs from Icarus\n--- icarus\n${icarus_last}--- msim\n${last_ref}")
endif()
function(expect_stable label)
  run_msim(got ${sv} ${ARGN})
  if(NOT got STREQUAL last_ref)
    message(FATAL_ERROR "lastwin/${label} is not stable\n--- threads-1\n${last_ref}--- got\n${got}")
  endif()
endfunction()
expect_stable(threads-2 -threads 2)
expect_stable(threads-3 -threads 3)
expect_stable(threads-4 -threads 4)
expect_stable(threads-default)
expect_stable(event-driven -threads 1 -no-static-comb -no-static-seq -no-partition)
expect_stable(threads-4-repeat -threads 4)
if(icarus_last STREQUAL last_ref)
  message(STATUS "lastwin matches Icarus, including which NBA won")
else()
  message(STATUS "lastwin hit matches Icarus and is stable across thread counts; the surviving same-slot write differs (LRM leaves that order unspecified)\n--- msim\n${last_ref}--- icarus\n${icarus_last}")
endif()

if(ncpu LESS 2)
  message(STATUS "host processors: ${ncpu}. msim caps -threads at that, so this run never leaves the calling thread.")
else()
  message(STATUS "host processors: ${ncpu}. Requested -threads 1..4; msim caps at the hardware thread count.")
endif()
